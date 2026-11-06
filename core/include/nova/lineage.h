#pragma once

// Self-replicator detection (design.md 6.6; Phase 1b plan D2): a pure observer.
//
// One Lineages per soup. The soup tells it about new blocks, freed blocks and
// DIVIDEs; it never writes to the soup or draws from its RNG, so the world
// hash is identical with detection on or off (a test checks this).
//
// Inheritance links come two ways, each counted over same-offset matches /
// the longer length, against `detect.similarity` (two random blocks match on
// about 1.6%):
//   - DIVIDE: the daughter block matches the block the dividing CPU runs in,
//     counting only codons the parent's body wrote into her since her ALLOC
//     (`detect.written`, default on). It tells "she was copied" apart from "a
//     random block was budded off" (random bodies bud a dozen times a tick) and
//     from "she was budded onto a relative's intact remains".
//   - INJECT (`detect.inject`, default on): the codons an INJECT just wrote
//     leave the victim's first block matching the injector's block. The victim
//     joins the injector's lineage as its offspring. This is how the first
//     replicators found on the home rig copy themselves: bud a blank daughter,
//     then inject their own code into her (design.md 6.6).
//
// Lineage: the first link from an untagged parent founds a lineage, with that
// parent as its depth-0 founder; every inheriting daughter (or infected victim)
// joins her parent's lineage one generation deeper. A member re-infected by
// its own lineage stays as it was; a member of another lineage that is taken
// over moves. Mutants are full members: the rules never check exactness, and
// neither does the detector.
//
// Gate (Phase 1b): a lineage in which an inheriting daughter has herself
// produced an inheriting daughter (depth >= detect.gate_depth, 2), with at
// least detect.gate_alive (2) members alive at once. Depth and the living count
// only grow at links, so the check runs there, at the exact moment within the
// tick.
//
// Members are blocks, not bodies: a replicator engulfed by an eater keeps
// running and stays a member.
//
// GPU links (Phase 2): a DIVIDE link is a GPU link when most of the daughter's
// matching codons were written by GPU ops. The first GPU link of a lineage
// that has met the gate is the `gpu_replicator` event: the milestone "first
// lineage that uses the GPU" (design.md 6.7), read as copying with it.
//
// NPU use (Phase 2b): the first NPU op done by a member of a lineage that has
// met the gate is the `npu_user` event (design.md 6.8).
//
// Across nodes (Phase 3): lineage ids are world-unique -- the founding node in
// the top 32 bits, a per-node count below (written n.k). A daughter dispersing
// to a neighbour carries her tag in her cargo and is adopted on arrival, so a
// lineage spans nodes; each node's detector holds the members on its node,
// and the gate is checked per node. The first arrival of a gated lineage on
// each node other than its founding one is a `colonist` event (design.md 6.9).
//
// Exact genotypes go to a per-soup genebank (genebank.h).
//
// What a Lineages produces during a tick (gate events, genebank saves, extinct
// lineages) waits in its own buffers. The run drains them at the barrier in
// node order, so every output is the same at any thread count.
//
// Not carried in snapshots: a resumed run starts detection afresh.
#include "nova/array.h"
#include "nova/config.h"
#include "nova/genebank.h"
#include "nova/hashmap.h"
#include "nova/soup.h"
#include "nova/types.h"

namespace nova {

struct DetectParams {
    bool enabled = false;
    double similarity = 0.75;    // detect.similarity
    u32 genebank_threshold = 2;  // detect.genebank_threshold
    bool inject = true;          // detect.inject: INJECT can make an inheritance link
    bool written = true;         // detect.written: DIVIDE links count only written codons
    u32 gate_depth = 2;          // detect.gate_depth: generations the gate needs
    u32 gate_alive = 2;          // detect.gate_alive: members alive at once it needs
    u32 signal_sustained = 100;  // signal.sustained: EMIT ticks for the signaller event (Phase 4)
    u32 defended = 3;            // detect.defended: defences for the defended event (Phase 9)
};

inline constexpr u32 kViaDivide = 0;
inline constexpr u32 kViaInject = 1;

// Reads detect.* (marking the keys used); a bad value is reported through cfg.
bool detect_params_from_config(Config& cfg, DetectParams* p);

struct BlockTag {
    u64 genotype; // codon hash when the block joined its lineage
    u64 birth;    // tick it joined
    u64 lineage;  // world-unique id (founding node << 32 | count); 0 = not a member
    u32 depth;    // generations from the founder (0)
    u32 pad;
};

// Lineage ids: the founding node in the top 32 bits.
inline u32 lineage_node(u64 id) { return static_cast<u32>(id >> 32); }
// Writes an id as "node.count".
void append_lineage(TextBuf& out, u64 id);

struct LineageInfo {
    u64 founded;          // tick
    u64 last_birth;       // tick of the latest inheriting birth
    u64 founder_genotype;
    u64 members;          // members ever, founder included
    u64 divide_links;     // links by DIVIDE and by INJECT, re-infections included
    u64 inject_links;
    u64 gpu_links;        // DIVIDE links whose copying the GPU mostly did
    u32 alive;
    u32 peak_alive;
    u32 max_depth;
    u32 founder_len;
    u32 detected;         // the gate has been met
    u32 gpu_detected;     // ... and then a GPU link was made
    u32 npu_detected;     // ... and then a member did an NPU op
    u32 disk_detected;    // ... and then a member restored its own code from disk (Phase 4)
    u64 last_emit;        // tick + 1 of a member's latest EMIT once gated, 0 = none (Phase 4)
    u32 emit_ticks;       // distinct ticks on which a member emitted, once gated
    u32 signal_detected;  // ... reached signal.sustained
    u32 native_detected;  // ... and then a GPU-born member made an inheriting daughter (Phase 8)
    u32 defences;         // actions aimed at members that failed because they changed in time, once gated (9)
};

inline constexpr u32 kEventReplicator = 0;    // the lineage met the gate
inline constexpr u32 kEventGpuReplicator = 1; // a gated lineage copied with the GPU
inline constexpr u32 kEventNpuUser = 2;       // a gated lineage's member used the NPU
inline constexpr u32 kEventColonist = 3;      // a gated lineage reached another node
inline constexpr u32 kEventDiskUser = 4;      // a gated lineage's member loaded its own code from disk
inline constexpr u32 kEventSignaller = 5;     // a gated lineage's members emitted on signal.sustained ticks
inline constexpr u32 kEventGpuNative = 6;     // a gated lineage's GPU-born member made an inheriting daughter (8)
inline constexpr u32 kEventDefended = 7;      // a gated lineage's members made detect.defended attacks fail (9)
inline constexpr u32 kEventCount = 8;

// The first time a lineage meets the gate, first makes a GPU link after, first
// has a member use the NPU, or first arrives on a node it wasn't founded on.
struct GateEvent {
    u64 tick;
    u64 genotype; // the daughter (or victim, or arrival) it was about
    u64 lineage;
    u32 depth, alive, len;
    u32 codon_off; // her genome, in codons()
    u32 via;       // kViaDivide or kViaInject
    u32 kind;      // kEvent...
};

// A genotype saved to the genebank.
struct GenebankSave {
    u64 tick;
    u64 genotype;
    u64 parent;        // 0 = a lineage founder
    u64 lineage;
    u32 len;
    u32 name;          // Genebank name index
    u32 parent_name;   // Genebank::kNoName if the parent is unsaved (or none)
    u32 codon_off;     // the genome, in codons()
};

// A lineage that went extinct on this node (or, at the end of a run, one
// still alive here).
struct LineageEnd {
    LineageInfo info;
    u64 ended;
    u64 id;
};

// Window summary (CSV columns).
struct LineageSurvey {
    u64 top_lineage;    // id of the lineage with the most living members (0 = none)
    u32 max_depth;      // deepest living member
    u32 lineages;       // lineages with a living member
    u32 top_alive;      // its living members
    u32 top_genotypes;  // distinct genotypes among them
    u32 colonists;      // living members of lineages founded on another node
    u32 pad;
};

class Lineages {
  public:
    void init(const DetectParams* p, u32 node);
    // Drops every tag and written mark (the soup calls this when it is
    // attached, and again after a snapshot load); the lineage table, counters
    // and genebank are kept.
    void reset(usize block_slots, u32 soup_size);

    // --- hooks, called by the soup during its tick -------------------------
    void on_new_block(u32 k, u32 start, u32 len);
    void on_free_block(u32 k, u64 tick);
    // Phase 10: block k is a piece bitten off another block (its codons, and
    // what was written into them, carried over): untagged.
    void on_carve(u32 k) {
        if (k >= tags_.size())
            tags_.resize(static_cast<usize>(k) + 1);
        else
            tags_[k] = BlockTag{};
    }
    // A CPU wrote codon a (COPY, WRITE, movii) into its own body.
    void on_write(u32 a) {
        written_[a >> 6] |= 1ull << (a & 63);
        gpu_written_[a >> 6] &= ~(1ull << (a & 63));
    }
    // A GPU op wrote codon a into the issuing CPU's body.
    void on_write_gpu(u32 a) {
        written_[a >> 6] |= 1ull << (a & 63);
        gpu_written_[a >> 6] |= 1ull << (a & 63);
    }
    // parent_block is the block the dividing CPU runs in (kNone if lost).
    void on_divide(const Soup& s, u32 parent_block, u32 daughter_block, u64 tick);
    // An INJECT from the CPU running in injector_block wrote into the victim's
    // first block; `match` counts the written codons that equal the injector's
    // codon at the same offset.
    void on_inject(const Soup& s, u32 injector_block, u32 victim_block, u32 match, u64 tick);
    // A CPU running in `block` did an NPU op (Phase 2b).
    void on_npu(const Soup& s, u32 block, u64 tick);
    // Phase 3: the tag a daughter in block k carries when she disperses, and
    // her adoption on arrival in block k of this node.
    CargoTag cargo_tag(u32 k) const;
    void on_arrive(const Soup& s, u32 k, const CargoTag& tag, u64 tick);
    // Phase 4: a volatility wipe turned [a, a + n) into random codons (they
    // are no longer anyone's writing); a DISK load by the CPU running in
    // `block` wrote into that block (restoring its own code).
    void on_wipe(u32 a, u32 n) {
        for (u32 i = a; i < a + n; ++i) {
            written_[i >> 6] &= ~(1ull << (i & 63));
            gpu_written_[i >> 6] &= ~(1ull << (i & 63));
        }
    }
    void on_disk_load(const Soup& s, u32 block, u64 tick);
    // The node came back after a collapse (Phase 4 lifecycle): a new node, so
    // gated lineages arriving can colonise it again. (Its members all ended at
    // the collapse; the soup calls reset when it is attached again.)
    void new_life() { colonised_.clear(); }
    // An EMIT by the CPU running in `block` (Phase 4).
    void on_emit(const Soup& s, u32 block, u64 tick);
    // Phase 9: an ENGULF or INJECT aimed at the body whose first block is
    // `block` failed at its end because the body changed in time (its
    // surface, its place, its size).
    void on_defended(const Soup& s, u32 block, u64 tick);

    // --- inspection --------------------------------------------------------
    u32 node() const { return node_; }
    u64 links() const { return links_; }     // inheritance links so far, both kinds
    u64 inject_links() const { return inject_links_; }
    u64 gpu_links() const { return gpu_links_; }
    u64 gpu_detections() const { return gpu_detections_; }
    u64 npu_detections() const { return npu_detections_; }
    u64 colonisations() const { return colonisations_; }
    u64 disk_detections() const { return disk_detections_; }
    u64 signal_detections() const { return signal_detections_; }
    u64 defended_detections() const { return defended_detections_; }
    bool is_written(u32 a) const { return (written_[a >> 6] >> (a & 63)) & 1; }
    bool is_gpu_written(u32 a) const { return (gpu_written_[a >> 6] >> (a & 63)) & 1; }
    u64 founded() const { return founded_; } // lineages founded so far
    u64 detections() const { return detections_; }
    u32 max_depth_ever() const { return max_depth_ever_; }
    usize living() const { return table_.size(); }
    const LineageInfo* lineage(u64 id) const { return table_.find(id); }
    BlockTag tag(u32 k) const { return k < tags_.size() ? tags_[k] : BlockTag{}; }
    const Genebank& genebank() const { return bank_; }
    void survey(LineageSurvey* out) const;
    // Every living lineage as a LineageEnd ending at `tick` (end-of-run report),
    // in lineage-id order.
    void living_report(u64 tick, Array<LineageEnd>* out) const;

    // --- barrier outputs (drained by the run, then cleared) -----------------
    const Array<GateEvent>& gates() const { return gates_; }
    const Array<GenebankSave>& saves() const { return saves_; }
    const Array<LineageEnd>& ends() const { return ends_; }
    const u8* codons() const { return codons_.data(); }
    void clear_outputs();

  private:
    bool similar(u32 match, u32 len_a, u32 len_b) const;
    void link(const Soup& s, u32 parent_block, u32 child_block, u64 tick, u32 via, bool gpu);
    void push_event(u32 kind, u64 tick, u64 lineage, const LineageInfo& L, const BlockTag& child,
                    const u8* code, u32 len, u32 via);
    void join(u32 k, const u8* code, u32 len, u64 tick, u64 lineage, u32 depth, u64 parent);
    void leave(u32 k, u64 tick);
    u32 stash(const u8* code, u32 len);

    const DetectParams* p_ = nullptr;
    u32 node_ = 0;
    u32 next_id_ = 1;
    u64 links_ = 0, inject_links_ = 0, gpu_links_ = 0, founded_ = 0, detections_ = 0;
    u64 gpu_detections_ = 0, npu_detections_ = 0, colonisations_ = 0, disk_detections_ = 0;
    u64 signal_detections_ = 0, defended_detections_ = 0;
    u32 max_depth_ever_ = 0;
    Array<BlockTag> tags_;   // by block slot
    Array<u64> written_;     // per codon: written by its owner since its block was allocated
    Array<u64> gpu_written_; // ... last by a GPU op
    HashMap<LineageInfo> table_; // living lineages only
    HashMap<u32> colonised_;     // lineages from elsewhere that have colonised this node
    Genebank bank_;

    Array<GateEvent> gates_;
    Array<GenebankSave> saves_;
    Array<LineageEnd> ends_;
    Array<u8> codons_;
};

} // namespace nova
