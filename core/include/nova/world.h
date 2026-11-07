#pragma once

// The Phase 0 world: the tick loop, barrier and per-edge outboxes (design.md
// 7.1 D), carried by a deliberately trivial "demo" node model.
//
// The node model is a placeholder; Phase 1a replaces it with soups of bodies
// and Phase 3 with procedurally generated nodes. What it exercises is exactly
// the machinery those phases inherit:
//
//   update phase (parallel, one task per node)
//     Node n reads and writes only its own state: its codon soup, its unit pool
//     and the outboxes of its own out-edges. Every random draw comes from
//     Rng(seed, n, tick). It mutates a few codons and sends "parcels" (units plus
//     one codon) along random out-edges. Each per-edge outbox has a fixed
//     capacity -- edge capacity caps simultaneous transfers (4.4a F) -- so a send
//     into a full outbox is refused and the units stay home.
//
//   barrier, then delivery phase (serial, fixed order)
//     Destinations in node-id order; for each, its incoming edges by source id;
//     for each edge, parcels in the order they were sent. A destination accepts
//     units only up to its free capacity and the excess bounces back to the
//     sender, so delivery order genuinely changes the outcome: the determinism
//     gate therefore tests the fixed order rather than passing by accident.
//     (Same shape as the dispersal exchange, 4.4a E: "capped by what's there".)
//
// Units are conserved: they move between pools and never appear or vanish.
// Capacity limits arrivals only; a bounce can leave a sender above capacity.
//
// Phase 1a adds a second node model, selected by `world.model`:
//
//   demo  (default) the parcel model above, kept byte-identical so the Phase 0
//         gate hash, replay logs and snapshots stay valid.
//   soup  every node is a Soup of organisms (soup.h). The update phase runs one
//         Soup::tick per node; nothing crosses edges until dispersal (Phase 3),
//         so delivery only sums the per-node tallies.
//
// Phase 1b adds the replicator detector (lineage.h): one Lineages per soup when
// detect.enabled (the default), watching but never touching the world.
//
// Phase 3 (design.md 6.9) adds the world proper, in the soup model:
//
//   world.graph = ring (default: a ring plus random chords, as before) or
//   procedural (graph.h: a hidden 2D space, proximity links, ruggedness, long
//   links). Every node has its own SoupParams; in a procedural world the
//   vary.* knobs spread them over noise fields (node value = mean x (1 + f x
//   noise), noise -1..1).
//
//   Dispersal (world.dispersal, default on only in procedural worlds): DIVIDE
//   to neighbour k (1 = nearest) queues a departure on the node during its
//   update. At the barrier the world moves each node's departures, in node
//   order, onto its out-edges as cargo (a global send sequence number), then
//   delivers every cargo due at this tick in the fixed order: destination id,
//   then source id, then send order. A cargo of L codons sent during tick t is
//   due at t + ceil(L / bandwidth) + latency: placed at the barrier that ends
//   that tick, she runs from the next. An edge carries at most edge.capacity
//   cargoes at once. Cargo is world state (hashed, snapshotted); the lineage
//   tag it carries is detector data and is in neither.
//
//   world.origins = all (a soup with sparks everywhere) or one (only node 0
//   gets sparks; the rest are lifeless random waste until colonised).
//   seed.node = all, or k for one seeded founder on node k.
#include "nova/accel.h"
#include "nova/accelop.h"
#include "nova/array.h"
#include "nova/config.h"
#include "nova/graph.h"
#include "nova/io.h"
#include "nova/landscape.h"
#include "nova/lineage.h"
#include "nova/pool.h"
#include "nova/soup.h"
#include "nova/types.h"

namespace nova {

struct Parcel {
    u32 amount;
    u8 codon;
    u8 pad[3];
};

// Counters for the most recent tick (stats input).
struct TickStats {
    u64 parcels_sent;
    u64 parcels_refused; // outbox full
    u64 parcels_delivered;
    u64 units_accepted;
    u64 units_bounced;
    u64 mutations;
};

// A world event at the barrier (Phase 4 lifecycle): a node collapsed, or a
// site became a node (again). The run logs them as replay events.
struct WorldEvent {
    u64 tick;
    u32 kind; // 0 collapse, 1 recovered
    u32 node;
    u32 life; // the node's lives so far
    u32 pad;
};

// Where a tick's time goes (Phase 7, `nova run --profile`): seconds summed over
// the ticks profiled. The update is the parallel part; the rest is the
// barrier, which runs on one thread, and the run's work after the tick.
struct StepProfile {
    u64 ticks = 0;
    double update = 0, accel = 0, signals = 0, departures = 0, delivery = 0, links = 0, lifecycle = 0, tally = 0,
           ports = 0, after = 0;
    // Phase 8, warps on the GPU: the parallel pack / unpack / host passes, and
    // the device's round (upload, the kernel, download) -- both outside update.
    double warp_cpu = 0, warp_gpu = 0;
    double warp_pack = 0, warp_host = 0; // the two halves of warp_cpu
    // Each node's tick by part (Soup::Timing), thread-seconds summed over the
    // nodes: they run in parallel, so these add up to more than the wall time.
    double n_first = 0, n_bodies = 0, n_pack = 0, n_ref = 0, n_unpack = 0, n_host = 0, n_second = 0;
    // warp_step's phases in wall time (Phase 10 profiling): every node's
    // first half, the packing, the split (choosing devices, fetching lanes,
    // building jobs), unpacking, the host's lanes, host instructions, every
    // node's second half, and later rounds' everything.
    double w_first = 0, w_pack = 0, w_split = 0, w_unpack = 0, w_hostlanes = 0, w_host = 0, w_second = 0, w_later = 0;
    double w_choose = 0, w_fetch = 0; // the split's first two parts: choosing devices, fetching lanes
    double w_first_max = 0, w_pack_max = 0, w_second_max = 0; // the slowest node's, each tick (summed)
};

enum class KnobResult { ok, unknown_key, not_live, bad_value };
enum class Model : u8 { demo, soup };
const char* describe(KnobResult r);

class World {
  public:
    World() = default;
    ~World() {
        delete[] lineages_;
        delete[] soups_; // closes (and deletes) any disk files
        if (disk_folder_.size())
            remove_folder(disk_folder_.c_str());
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Builds the world from config keys: seed, world.* (structural), and either
    // demo.* or the soup keys (soup.h). On a bad value returns false; the reason
    // is in cfg.value_error(). Keys read here are marked used.
    bool init(Config& cfg);

    // One tick: parallel update over all nodes, barrier, serial delivery.
    void step(ThreadPool& pool);
    // Times each part of every step into *p (nullptr = off).
    void set_profile(StepProfile* p); // (each node times its own parts too)
    StepProfile* profile() const { return prof_; }
    // The kernel spike's replay (docs/kernel-spike-plan.md K1): the first GPU
    // round of the tick that ends at `tick` (or later) is written to `path` as
    // the GPU receives it (that round is verified, so every lane it needs is
    // on the host), for `nova bench warp`. The world runs on unchanged.
    void set_warp_dump(const char* path, u64 tick) {
        warp_dump_path_ = path;
        warp_dump_tick_ = tick;
    }

    // Live knobs (demo.* in the demo model; spark.rate, spark.grant,
    // spark.min_len, spark.max_len, and from Phase 6 cpu.wear, heat.ambient
    // and, with dispersal, edge.corruption in the soup model) may change
    // between ticks; structural keys may not. check_knob validates without
    // applying.
    KnobResult check_knob(Str key, Str value) const;
    KnobResult set_knob(Str key, Str value);
    // The soup model's live knobs, for a knobs panel: their keys and current
    // (mean) values.
    u32 knob_count() const;
    Str knob_key(u32 i) const;
    double knob_now(u32 i) const;

    // Fingerprint of the full world state: seed, tick, graph, pools, soups.
    u64 hash() const;

    Model model() const { return model_; }
    // Soup model only.
    const Soup& node_soup(u32 node) const { return soups_[node]; }
    Soup& node_soup(u32 node) { return soups_[node]; }
    // The mean (configured) parameters, and node n's own (Phase 3).
    const SoupParams& soup_params() const { return soup_params_; }
    const SoupParams& node_params(u32 node) const { return node_params_[node]; }
    const SoupTally& last_soup_tick() const { return soup_last_; }
    // The node's replicator detector, or nullptr when detection is off.
    const Lineages* node_lineages(u32 node) const { return lineages_ ? &lineages_[node] : nullptr; }
    Lineages* node_lineages(u32 node) { return lineages_ ? &lineages_[node] : nullptr; }
    const DetectParams& detect_params() const { return detect_; }

    u64 seed() const { return seed_; }
    u64 tick() const { return tick_; }
    u32 node_count() const { return nodes_; }
    u32 edge_count() const { return static_cast<u32>(edge_src_.size()); }
    u32 soup_size() const { return soup_size_; }
    u32 pool_capacity() const { return pool_capacity_; }
    u32 pool(u32 node) const { return pool_[node]; }
    const u8* soup(u32 node) const { return soup_.data() + static_cast<usize>(node) * soup_size_; }
    u64 total_units() const;
    const TickStats& last_tick() const { return last_; }

    // Graph (directed edges, sorted by (src, dst); each undirected link is two edges).
    u32 edge_src(u32 e) const { return edge_src_[e]; }
    u32 edge_dst(u32 e) const { return edge_dst_[e]; }
    u32 out_begin(u32 node) const { return out_begin_[node]; }
    u32 out_end(u32 node) const { return out_begin_[node + 1]; }
    u32 in_count(u32 node) const { return in_begin_[node + 1] - in_begin_[node]; }
    u32 in_edge(u32 node, u32 k) const { return in_list_[in_begin_[node] + k]; }

    // Phase 3. Hidden positions (procedural and landscape worlds; 0 otherwise).
    bool procedural() const { return procedural_; }
    bool dispersal() const { return dispersal_; }
    double node_x(u32 node) const { return spatial_ ? node_x_[node] : 0.0; }
    double node_y(u32 node) const { return spatial_ ? node_y_[node] : 0.0; }
    // Phase 7: a landscape (landscape.h): the world is world_width() x 1, with
    // its land raster, landmasses, and bridges that open on a schedule.
    bool landscape() const { return landscape_; }
    bool spatial() const { return spatial_; }
    const Landscape& land() const { return ls_; }
    const LandParams& land_params() const { return lp_; }
    double world_width() const { return landscape_ ? ls_.width : 1.0; }
    u32 node_mass(u32 node) const { return landscape_ ? ls_.mass[node] : 0; }
    // --- a varied world (docs/world-plan.md W1, W2) ---------------------------
    // Substrates: on in procedural and landscape soup worlds with
    // substrate.enabled (a ring has no map: every level 1x). Node n's level
    // (0-4, landscape.h) of each type, the one its current life took from the
    // fields at its site (kSubstrateOne, 1x, when off), and its multiplier.
    bool substrates() const { return substrates_; }
    const SubstrateParams& substrate_params() const { return sub_params_; }
    u8 substrate_level(u32 node, u32 type) const {
        return substrates_ ? sub_level_[static_cast<usize>(node) * kSubstrateTypes + type] : kSubstrateOne;
    }
    double substrate_mult(u32 node, u32 type) const {
        return substrate_multiplier(substrate_level(node, type), sub_params_.floor_mult);
    }
    bool edge_bridge(u32 e) const { return landscape_ && edge_bridge_[e]; }
    bool edge_shut(u32 e) const; // a bridge, shut this tick
    u32 edge_phase(u32 e) const { return landscape_ ? edge_phase_[e] : 0; }
    // Node n's out-edge to its neighbour number k + 1 (nearest first).
    u32 degree(u32 node) const { return out_end(node) - out_begin(node); }
    u32 port(u32 node, u32 k) const { return port_list_[out_begin_[node] + k]; }
    // Per directed edge (dispersal worlds).
    u32 edge_latency(u32 e) const { return edge_latency_[e]; }
    u32 edge_bandwidth(u32 e) const { return edge_bandwidth_[e]; }
    double edge_distance(u32 e) const { return spatial_ ? edge_dist_[e] : 0.0; }
    u32 edge_capacity() const { return edge_capacity_; }
    u32 edge_in_flight(u32 e) const { return in_flight_[e]; }
    u64 in_flight() const { return cargo_.size() - drifting(); } // daughters in transit now
    u64 drifting() const {                                         // dormant programs in transit now
        u64 n = 0;
        for (usize i = 0; i < cargo_.size(); ++i)
            n += cargo_[i].dormant;
        return n;
    }
    // Cargo i in transit (send order): its edge, the tick it is due, and its
    // transit time in ticks (so it left at the barrier ending due - ticks).
    u32 cargo_edge(usize i) const { return cargo_[i].edge; }
    u64 cargo_due(usize i) const { return cargo_[i].due; }
    u32 cargo_ticks(usize i) const { return cargo_[i].ticks; }
    u32 cargo_len(usize i) const { return cargo_[i].len; }
    u32 links_cut() const { return links_cut_; }
    u32 long_links() const { return long_added_; }
    // Phase 4: links down now (undirected), and whether edge e is down.
    u64 links_down() const;
    bool edge_down(u32 e) const { return e < link_down_.size() && link_down_[e]; }
    u64 edge_down_until(u32 e) const { return e < down_until_.size() ? down_until_[e] : 0; }
    // Live nodes (Phase 4 lifecycle; every node otherwise).
    u64 live_nodes() const;
    bool lifecycle() const { return lifecycle_; }
    bool live(u32 n) const { return live_.empty() || live_[n]; }
    u32 life(u32 n) const { return life_.empty() ? 0 : life_[n]; }
    u64 born(u32 n) const { return born_.empty() ? 0 : born_[n]; }
    u64 collapses_total() const { return collapses_total_; }
    u64 recoveries_total() const { return recoveries_total_; }
    // Diagnostics since this World object started (not in snapshots): the
    // collapsed nodes' ages summed (ticks since each was born or returned),
    // and the bodies they took with them.
    u64 collapse_age_sum() const { return collapse_age_; }
    u64 collapse_bodies_sum() const { return collapse_bodies_; }
    u64 arrived_dry_sum() const; // every node's Soup::arrived_dry, summed
    u64 arrived_sum() const;     // every node's Soup::arrived_all, summed
    const Array<WorldEvent>& world_events() const { return wevents_; }
    void clear_world_events() { wevents_.clear(); }
    // Phase 7b: the folder holding the disk tier's files ("" = in RAM).
    Str disk_folder() const { return disk_folder_.view(); }
    // Phase 5: asynchronous accelerator ops in flight.
    bool accel_async() const { return accel_async_; }
    u64 accel_in_flight() const { return acc_ops_.size(); }
    // Where they run (accel.gpu, accel.npu): a device's name, or "cpu" for the
    // CPU reference. Never changes a result. Ops completed on a device and by
    // the reference; ops checked against the reference (accel.verify) and how
    // many differed (the reference's answer is used). Notes: a device that
    // failed to start or failed mid-run, and the first difference found.
    const char* accel_gpu_name() const;
    const char* accel_npu_name() const;
    u64 accel_device_ops() const { return acc_device_ops_; } // of which on the NPU:
    u64 accel_npu_ops() const { return acc_npu_ops_; }
    u64 accel_reference_ops() const { return acc_reference_ops_; }
    u64 accel_verified() const { return acc_verified_; }
    u64 accel_mismatches() const { return acc_mismatches_; }
    Str accel_notes() const { return acc_notes_.view(); }
    // Phase 8: warps (design.md 6.14). Where they run (warp.device): the
    // GPU's name, or "cpu" for the reference. Never changes a result. Rounds
    // run on the device and by the reference; rounds checked against the
    // reference (warp.verify) and how many differed (the reference's result
    // is kept). Notes: a device that failed to start or failed mid-run.
    bool warps() const { return model_ == Model::soup && soup_params_.warps; }
    const char* warp_device_name() const { return warp_gpu_.is_open() ? warp_gpu_.name() : "cpu"; }
    u64 warp_gpu_rounds() const { return warp_gpu_rounds_; }
    u64 warp_ref_rounds() const { return warp_ref_rounds_; }
    u64 warp_verified() const { return warp_verified_; }
    u64 warp_mismatches() const { return warp_mismatches_; }
    Str warp_notes() const { return warp_notes_.view(); }
    double warp_compile_ms() const { return warp_compile_ms_; }
    const WarpGpuStats& warp_gpu_stats() const { return warp_gpu_.stats(); }
    // Which warp kernel the GPU runs (DXC's or FXC's, its tiers, and why FXC), or "".
    const char* warp_kernel() const { return warp_gpu_.is_open() ? warp_gpu_.kernel() : ""; }
    // warp.device = auto: the GPU and the CPU reference each take a share of
    // the bodies every round, at once. A body goes to the CPU when its last
    // first round ran more than warp_limit() steps (the GPU runs a body's
    // steps one after another); the limit is searched for, round by round, as
    // the one that makes a round quickest per warp step. Body-rounds run on
    // each so far. Auto also times whole ticks run so against ticks without
    // the GPU at all (the plain reference, as warp.device = cpu), and runs the
    // quicker; warp_plain_ticks() counts the ticks it ran without the GPU.
    bool warp_auto() const { return warp_auto_; }
    u32 warp_limit() const { return warp_limit_; }
    u64 warp_gpu_bodies() const { return warp_gpu_bodies_; }
    u64 warp_ref_bodies() const { return warp_ref_bodies_; }
    // The split's model (seconds a GPU serial step, seconds a reference step
    // on one thread; 0 until measured), and the CPU threads' wall time on
    // their share of the GPU rounds so far, in seconds.
    double warp_model_gstep() const { return warp_gstep_; }
    double warp_model_hstep() const { return warp_hstep_; }
    double warp_ref_wall() const { return warp_ref_wall_; }    u64 warp_plain_ticks() const { return warp_plain_ticks_; }
    u64 warp_split_ticks() const { return warp_auto_ticks_ - warp_plain_ticks_; }
    // The GPU failed while it held warps' lanes: the world stopped (it can't
    // go on exactly); resume from a snapshot.
    bool warp_failed() const { return warp_failed_; }
    // Every warp's lanes back from the GPU (kNone: on every node), so the host
    // can read them. hash() and save_state() do it themselves.
    void warp_sync(u32 node = kNone);
    // Signals (Phase 4): channels, and node n's signal paths (sources it
    // reaches: target, hops, delay).
    u32 channels() const { return channels_; }
    u32 route_count(u32 n) const { return route_begin_.size() ? route_begin_[n + 1] - route_begin_[n] : 0; }
    u32 route_target(u32 n, u32 i) const { return route_tgt_[route_begin_[n] + i]; }
    u32 route_hops(u32 n, u32 i) const { return route_hops_[route_begin_[n] + i]; }
    u32 route_delay(u32 n, u32 i) const { return route_delay_[route_begin_[n] + i]; }

    // Snapshot support. Call load_state() on a world freshly init()ed from the
    // snapshot's config; it replaces tick, graph, pools and soups.
    // `version`: the snapshot format's (snapshot.h); older ones lack a part.
    void save_state(ByteWriter& w) const;
    bool load_state(ByteReader& r, TextBuf* err, u32 version = 2);

  private:
    struct Tally {
        u32 sent, refused, mutations, pad;
    };

    // A daughter in transit (Phase 3). Raw-hashable except the tag.
    struct Cargo {
        u64 due;   // delivered at the barrier ending this tick
        u64 seq;   // world send order
        CargoTag tag; // detector data: not hashed, not snapshotted
        u32 edge, len, codon_off;
        u32 millicores, gpus, npus;
        u32 ticks; // in transit (Phase 4: corruption exposure); derived from the edge
        u32 disk;  // disk codons she claims on arrival (Phase 4)
        u32 form;  // kFormCpu or kFormGpu (Phase 8)
        u32 emillicores; // her E millicores (Phase 9c; millicores is then P)
        u32 kind;  // the kind of core her thread prefers (Phase 9c)
        // A dormant program drifting (docs/dormancy-plan.md): the cores and disk
        // above are what it held at home; it travels outside the edge's capacity.
        u32 dormant;     // 1: a dormant program, 0: a daughter
        u32 mem_reserve; // its memory reserve (its disk reserve is `disk`)
        u32 hops;        // hops so far, this one included
    };
    static_assert(sizeof(Cargo) == 104, "Cargo must have no implicit padding");

    bool build_edges(const u64* keys, usize n); // keys: (src << 32 | dst), strictly increasing
    void update_node(u32 node);
    void deliver();
    static void update_task(void* ctx, u32 node);
    // Phase 3
    // Per-node SoupParams: vary.* factors (by NoiseField), world.origins = one,
    // seed.node (kNone = all).
    void init_nodes(const double* vary, bool one_origin, u32 seed_node);
    void init_ports();               // neighbour numbering and each soup's ports
    void take_departures();          // the barrier: departures onto edges
    void deliver_cargo();            // the barrier: due cargoes, in the fixed order
    // Dormancy and drift (docs/dormancy-plan.md): a dormant program due at the
    // barrier (land or pass through), a random open path out of a node, and
    // whether a path is open (not down, not to a fallow site).
    void deliver_dormant(Cargo& c);
    u32 drift_edge(u32 n, u64 seq, u32 hops) const;
    bool drift_open(u32 e) const;
    Array<Cargo> drift_out_;   // scratch: dormant programs passing through this barrier
    Array<u8> drift_codons_;   // ... and their codons
    u64 world_dormant_expired_ = 0, world_dormant_hops_ = 0; // this tick, added to the tally
    void apply_spark_rate(double rate);
    // Phase 4
    void corrupt(Cargo& c);          // transit corruption of one cargo, at delivery
    void links_update();             // the barrier: links failing and coming back
    // Removes every cargo on an edge with drop[edge] set, counting them lost.
    void drop_cargo(const u8* drop);
    void compute_routes();           // signal paths over the links that are up
    void signal_field(u32 node);     // the update phase: the field node hears this tick
    void scatter_signals();          // the barrier: this tick's emissions, onto their paths
    // The lifecycle: property fields at a tick (drifting when world.drift > 0),
    // one node's params from them (first = its first life), the land mask at a
    // tick, the barrier's collapses and recoveries, and rebuilding the graph
    // among live sites (cargo and link state carried across).
    void fields_at(u64 tick, Array<double>* z);
    void derive_node(u32 n, const Array<double>* z, bool first);
    void land_at(u64 tick, Array<u8>* mask);
    void lifecycle_update();
    void rebuild_graph();
    // Phase 5: the barrier's asynchronous accelerator ops: gather this tick's,
    // then complete those due. The devices: opening them (init), sending the
    // newly gathered ops from `first` on, and completing ops [lo, hi) that are
    // due (true if any were).
    void accel_barrier();
    void accel_open(Config& cfg);
    void accel_submit(usize first);
    bool accel_complete_range(usize lo, usize hi);

    // structural
    u64 seed_ = 0;
    u64 tick_ = 0;
    u32 nodes_ = 0;
    u32 soup_size_ = 0;
    u32 pool_capacity_ = 0;
    u32 outbox_capacity_ = 0;

    // live knobs
    double send_rate_ = 0;
    u32 sends_per_tick_ = 0;
    u32 mutations_per_tick_ = 0;
    u32 max_parcel_ = 0;

    // graph
    Array<u32> edge_src_, edge_dst_;
    Array<u32> out_begin_; // nodes + 1 offsets into the edge arrays
    Array<u32> in_begin_;  // nodes + 1 offsets into in_list_
    Array<u32> in_list_;   // edge ids grouped by dst, each group in src order

    // node state (flat: node n's soup is soup_[n * soup_size_ ...])
    Array<u8> soup_;
    Array<u32> pool_;
    Array<u64> rng_counter_; // each node's draw count after its update phase
    Array<Tally> tally_;

    // per-edge outboxes (edge e's parcels are outbox_[e * outbox_capacity_ ...])
    Array<Parcel> outbox_;
    Array<u32> outbox_count_;

    TickStats last_ = {};

    // soup model
    Model model_ = Model::demo;
    SoupParams soup_params_;        // the configured (mean) parameters
    Array<SoupParams> node_params_; // each node's own; soups point into it
    Array<double> spark_factor_;    // per node: its spark.rate multiplier
    Soup* soups_ = nullptr; // nodes_ soups (new[]: Soup holds Arrays)
    SoupTally soup_last_ = {};
    DetectParams detect_;
    Lineages* lineages_ = nullptr; // nodes_ detectors, or nullptr

    StepProfile* prof_ = nullptr;
    TextBuf disk_folder_; // Phase 7b: this run's disk files, or empty (RAM)
    // Phase 7: a landscape (generated once; the ports derive from it each tick)
    bool landscape_ = false, spatial_ = false;
    LandParams lp_;
    Landscape ls_;
    Array<u8> edge_bridge_;       // per edge: a bridge
    Array<u32> edge_phase_;       // per edge: a bridge's schedule phase
    Array<u32> bridge_list_;      // the bridges' edges
    Array<u8> shut_;              // per edge: a bridge shut this tick
    Array<u8> port_down_;         // per edge: refuses departures (failed, shut, fallow end)
    bool ports_dirty_ = false;
    Spread spreads_[7];           // spark, then res.memory, cpu, gpu, npu, disk, ecpu (9c)
    Array<u32> share_mem_, share_cpu_, share_gpu_, share_npu_, share_disk_;
    Array<double> spark_w_;
    Array<double> ecpu_w_;        // Phase 9c: each site's weight for its E millicores' share
    void read_land_params(Config& cfg);
    void read_spreads(Config& cfg);
    static bool resource_field(u32 f);
    void spread_resources();
    // A varied world (docs/world-plan.md W2): substrates. sub_level_: each
    // node's levels (node x kSubstrateTypes + type), taken by its current life
    // (derived from the config, so not saved as state: hashed, and checked
    // against a snapshot's). sub_at_: every site's levels from the fields as
    // they stand at sub_at_tick_ (static unless world.drift moves them).
    bool substrates_ = false;
    SubstrateParams sub_params_;
    Array<u8> sub_level_, sub_at_;
    u64 sub_at_tick_ = ~0ull;
    void read_substrates(Config& cfg);
    void substrates_at(u64 tick);
    void refresh_ports();
    void bridges_update();
    // Phase 3: the world (graph structure derives from the config)
    bool procedural_ = false;
    bool dispersal_ = false;
    Array<double> node_x_, node_y_;
    Array<u32> edge_latency_, edge_bandwidth_;
    Array<double> edge_dist_;
    Array<u32> port_list_; // per node (offsets out_begin_): out-edges nearest first
    u32 edge_capacity_ = 0;
    u32 links_cut_ = 0, long_added_ = 0;
    // transit (world state)
    Array<Cargo> cargo_;      // in send order
    Array<u8> cargo_codons_;
    Array<u32> in_flight_;    // per edge, derived from cargo_
    u64 next_seq_ = 0;
    Array<u32> due_scratch_;
    Array<u64> order_scratch_;

    // Phase 4: edges (corruption, link failures)
    double edge_corruption_ = 0, edge_failure_ = 0, edge_downtime_ = 1000;
    Array<u32> edge_rev_;        // the opposite direction of each edge
    Array<u64> down_until_;      // per edge: down until this tick (0 = up); world state
    Array<u8> link_down_;        // per edge: 1 while down (ports read it)
    u64 world_corrupted_ = 0, world_transit_lost_ = 0; // this tick, added to the tally
    Array<u8> drop_scratch_;
    // Phase 4: the lifecycle (empty when off: every node is live)
    Array<u8> live_;
    bool lifecycle_ = false;
    double land_ = 1.0, drift_ = 0.0;
    u32 budget_ = 0, recover_every_ = 100;
    u64 fallow_min_ = 2000;
    double vary_[kFieldCount] = {};
    bool one_origin_ = false;
    u32 seed_node_ = 0xFFFFFFFFu;
    GraphParams gp_;
    ProcGraph pg_;            // the generation: sites, ruggedness, long pairs
    Array<u64> born_;         // per site: the tick it last became a node (0 = at the start)
    Array<u64> fallow_since_; // per site: when it went fallow (0 = sea from the start)
    Array<u32> life_;         // per site: lives after the first
    Array<u8> lived_;         // per site: has been a node
    u64 collapses_total_ = 0, recoveries_total_ = 0, world_collapses_ = 0, world_recoveries_ = 0;
    u64 collapse_age_ = 0, collapse_bodies_ = 0; // diagnostics
    Array<WorldEvent> wevents_;
    // Phase 4: signals
    u32 channels_ = 0;
    double attenuation_ = 0.25;
    u32 reach_ = 8;
    double latency_scale_ = 0;
    u32 sig_slots_ = 0;      // the pending ring: longer than any path's delay
    Array<double> pending_;  // [slot][node][channel]: what each node will hear; world state
    Array<double> att_pow_;  // attenuation^hops
    Array<u32> route_begin_; // per source node (nodes + 1 offsets)
    Array<u32> route_tgt_, route_hops_, route_delay_;
    Array<u32> bfs_dist_, bfs_delay_, bfs_queue_;
    // Phase 5: asynchronous accelerator ops in flight (world state), in gather
    // order: their inputs (read at gather) and write-back addresses.
    bool accel_async_ = false;
    Array<AccelOp> acc_ops_;
    Array<u8> acc_in_;
    Array<u32> acc_addr_;
    Array<u8> acc_out_; // scratch: one op's output codons
    // Phase 5: the devices. Not world state: never hashed or saved, so the
    // world is the same whichever device (or none) computed it.
    struct AccelJob { // one batch on a device: GPU tickets first, then the NPU's
        Array<u8> codons;              // its outputs, op after op
        Array<u32> scalars, out_off;   // per op in the batch
        bool ready = false, ok = false, verify = false;
    };
    GpuBackend gpu_;
    NpuBackend npu_;
    bool npu_on_gpu_ = false;     // NPU ops may go to the GPU kernel (no NPU, or it failed)
    u32 verify_every_ = 0;        // accel.verify
    u64 acc_batches_ = 0, acc_device_ops_ = 0, acc_npu_ops_ = 0, acc_reference_ops_ = 0, acc_verified_ = 0,
        acc_mismatches_ = 0;
    TextBuf acc_notes_;
    Array<i32> acc_job_;  // per op in acc_ops_: its batch's ticket, -1 (the reference), or -2 (held, 9c)
    Array<u32> acc_slot_; // per op: its place in that batch (held: where its codons start in acc_held_)
    // Phase 9c: a batch comes home from its device at the barrier after it was
    // sent, and its results wait here until due (a long latency holds no
    // device slot). Not world state: the same results as the reference's.
    Array<u8> acc_held_;
    Array<u32> acc_held_scalar_; // per op
    Array<u8> acc_held_flags_;   // per op: 1 checked against the reference, 2 ran on the NPU
    void accel_bring_home(usize first); // every op before `first` still on a device
    bool accel_collect(i32 job);        // waits for a batch (true: its results are good)
    Array<u32> acc_pick_; // scratch
    AccelJob acc_jobs_[2 * kAccelSlots];
    void accel_take_job(u32 job); // after a submit: the picked ops belong to batch `job`

    // Phase 8: warps on the GPU. Not world state, like the devices above.
    WarpGpu warp_gpu_;
    bool warp_auto_ = false;   // warp.device = auto: the GPU and the CPU share each round
    bool warp_mixed_ = false;  // warp.device = mixed: a test, nodes split at random every round
    u32 warp_limit_ = 1000000000u; // auto: nodes whose longest body ran more steps go to the CPU
    int warp_dir_ = 1;         // auto's search for the limit: its direction, stride (2^stride a round),
    double warp_stride_ = 1.0; //   and the last round's cost (seconds a warp step) and tick
    double warp_cost_ = 0;
    u64 warp_cost_tick_ = 0;
    bool warp_climb_fresh_ = true; // auto: the next GPU round follows a tick without it (or a start)
    u64 warp_auto_ticks_ = 0;  // auto: ticks decided so far, and how many ran without the GPU
    u64 warp_plain_ticks_ = 0;
    double warp_tick_s_[2] = {0, 0}; // auto: recent seconds a tick, [0] with the GPU, [1] without
    u32 warp_last_plain_ = 2;        // auto: how the last tick ran (2: none yet)
    u32 warp_run_len_ = 0;           // auto: ticks run that way in a row
    u32 warp_probe_left_ = 0;        // auto: ticks still to run of a probe of the other way
    bool warp_probe_plain_ = false;  // ... and which way it is
    bool warp_go_plain();            // auto: this tick without the GPU?
    bool warp_failed_ = false;
    bool warp_verifying_ = false; // this round also runs on the reference (warp.verify)
    u32 warp_verify_ = 0;
    u32 warp_round_ = 0;
    double warp_compile_ms_ = 0;
    u64 warp_gpu_rounds_ = 0, warp_ref_rounds_ = 0, warp_verified_ = 0, warp_mismatches_ = 0;
    u64 warp_gpu_bodies_ = 0, warp_ref_bodies_ = 0;
    TextBuf warp_notes_;
    Array<u8> warp_active_;      // per node: still has warps to run this tick
    Array<u8> warp_on_gpu_;      // per node: the GPU runs some of its bodies this round
    Array<WarpJob> warp_jobs_;
    Array<u32> warp_job_node_;
    Array<u32> warp_send_;       // the jobs' lanes to send (warp slots)
    Array<u32> warp_job_recs_, warp_job_words_; // the jobs' records (the bodies the GPU runs) and their sizes
    Array<u32> warp_job_chain_;                 // ... and their expected chains (long ones are dispatched first)
    Array<u64> warp_vstate_;     // warp.verify: (node << 40 | slot << 8 | lane state) before the round
    Array<WarpFetch> warp_fetch_;
    Array<u32> warp_fetch_node_;
    Array<u32> warp_save_img_, warp_save_lanes_; // warp.verify: the packed round, before the GPU ran it
    const char* warp_dump_path_ = nullptr; // set_warp_dump
    u64 warp_dump_tick_ = 0;
    bool warp_write_dump(const Array<u32>& cst);
    Array<u64> warp_save_off_;   // per job: where its image and lanes start in those
    void warp_open(Config& cfg);
    void warp_step(ThreadPool& pool, bool use_gpu = true); // use_gpu false: every node on the reference
    void warp_fail(Str why);
    bool warp_fetch(Array<WarpFetch>& list, Array<u32>& nodes);
    static void warp_first_task(void* ctx, u32 node);
    static void warp_pack_task(void* ctx, u32 node);
    static void warp_ref_task(void* ctx, u32 node);
    // The reference's share of a round, record by record over the threads
    // (a node's records are independent): (node << 32 | record), the
    // heaviest nodes first, and each one's time when profiling.
    Array<u64> warp_ref_list_;
    Array<double> warp_ref_time_;
    Array<u8> warp_got_; // scratch: which waiting warps' lanes came back with their round
    void warp_ref_build();
    // The split's per-node passes (on the threads) and what they hand back.
    bool warp_use_gpu_ = false, warp_same_ = false;
    // Per node: its first half, first pack and second half last tick, in
    // seconds (the profile's slowest node; ordering the pool by them, slowest
    // first, gained nothing: those phases are bound by their total work).
    Array<double> warp_node_t_;
    Array<u32> warp_node_gpu_, warp_node_ref_, warp_node_guess_, warp_node_longest_;
    Array<u32> warp_node_nfetch_, warp_node_nsend_, warp_node_nrec_, warp_node_ofetch_, warp_node_osend_, warp_node_orec_;
    static void warp_choose_task(void* ctx, u32 n);
    static void warp_list_count_task(void* ctx, u32 n);
    static void warp_list_fill_task(void* ctx, u32 n);
    // auto's split from a model (world.cpp): the GPU's seconds a serial step
    // and the reference's a warp step on one thread (0 until measured), the
    // limit they give, and their measuring after a round.
    double warp_gstep_ = 0, warp_hstep_ = 0, warp_ref_wall_ = 0;
    u32 warp_gpu_guess_ = 0; // the longest chain guessed among the bodies the GPU took this tick
    Array<u64> warp_chains_; // scratch: the bodies' chains
    u32 warp_model_limit(u32 threads);
    bool warp_model_pick(u32 threads, double rest, u32* limit);
    void warp_model_measure(u32 round, bool cpu_ran);
    static void warp_ref_rec_task(void* ctx, u32 i);
    static void warp_unpack_task(void* ctx, u32 node);
    static void warp_host_task(void* ctx, u32 node);
    static void warp_second_task(void* ctx, u32 node);
};

} // namespace nova
