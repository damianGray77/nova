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
#include "nova/array.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/pool.h"
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

enum class KnobResult { ok, unknown_key, not_live, bad_value };
const char* describe(KnobResult r);

class World {
  public:
    // Builds the world from config keys: seed, world.* (structural) and demo.*
    // (live knobs). On a bad value returns false; the reason is in
    // cfg.value_error(). Keys read here are marked used.
    bool init(Config& cfg);

    // One tick: parallel update over all nodes, barrier, serial delivery.
    void step(ThreadPool& pool);

    // Live knobs (demo.*) may change between ticks; structural keys (seed,
    // world.*) may not. check_knob validates without applying.
    KnobResult check_knob(Str key, Str value) const;
    KnobResult set_knob(Str key, Str value);

    // Fingerprint of the full world state: seed, tick, graph, pools, soups.
    u64 hash() const;

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

    // Snapshot support. Call load_state() on a world freshly init()ed from the
    // snapshot's config; it replaces tick, graph, pools and soups.
    void save_state(ByteWriter& w) const;
    bool load_state(ByteReader& r, TextBuf* err);

  private:
    struct Tally {
        u32 sent, refused, mutations, pad;
    };

    bool build_edges(const u64* keys, usize n); // keys: (src << 32 | dst), strictly increasing
    void update_node(u32 node);
    void deliver();
    static void update_task(void* ctx, u32 node);

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
};

} // namespace nova
