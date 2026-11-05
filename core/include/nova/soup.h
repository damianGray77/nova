#pragma once

// Soup: one node's organisms (design.md 6, 7.1 C; exact rules in design.md 6.5).
//
// Memory is a flat array of six-bit codons. Ownership is per codon, recorded as
// the id of the *block* that owns it; a block is a contiguous range claimed in
// one go (a spark, an ALLOC), and belongs to one body. Recording blocks rather
// than bodies makes DIVIDE and ENGULF a relabel of a few blocks instead of a
// rewrite of thousands of codons, and gives the instruction pointer its wrap
// point: an IP (or head) that runs off the end of its block wraps to the
// block's start (6.4 #14). A codon owned by nobody is waste: anyone may read
// it, and ALLOC and sparks claim it.
//
// A body owns blocks, CPUs and CPU units (millicores). Every CPU unit and every
// 64-codon memory page carries its own heat (4.5, 6.4 #17). Each tick a body
// runs instructions in proportion to the speed of the units it owns, split
// equally among its running CPUs. A body with no running CPU -- none left
// un-halted, or no millicore to run on -- dies in the sweep at the end of the
// tick (6.4 #1).
//
// Everything a tick does is a function of (seed, node, tick) and the soup's own
// state, so a soup can be updated on any thread (design.md 7.1 D).
#include "nova/array.h"
#include "nova/config.h"
#include "nova/hash.h"
#include "nova/io.h"
#include "nova/isa.h"
#include "nova/rng.h"
#include "nova/types.h"

namespace nova {

inline constexpr u32 kNone = 0xFFFFFFFFu;
inline constexpr u32 kMaxStack = 16;
inline constexpr u32 kPageBits = 6; // memory heat unit: 64 codons, the "bank"
inline constexpr u32 kPage = 1u << kPageBits;

// Smooth heat response with staggered onsets (4.5, 6.4 #18): errors rise from
// `warm`, throttling starts at `hot`, and past `max` the unit is freed.
struct HeatCurve {
    float warm;      // error multiplier starts rising
    float hot;       // speed starts falling
    float max;       // the unit is released (keeping its heat)
    float speed_min; // speed factor reached at `max`
    float err_max;   // error multiplier reached at `max`
    float pad;

    float speed(float h) const;     // 1 at or below hot, down to speed_min at max
    float err_mult(float h) const;  // 1 at or below warm, up to err_max at max
};

struct SoupParams {
    // memory and compute
    u32 size;           // codons per node
    u32 millicores;     // CPU units per node
    u32 instr_per_core; // instructions per tick for 1000 millicores at full speed
    // instruction set
    CodonTable table;
    bool open_membrane; // light Tierra mode: read, search and execute anywhere
    u32 stack_depth;    // 1..kMaxStack
    u32 search_limit;   // template search distance, each way
    u32 max_template;   // longest template / surface considered
    u32 halt_after;     // K consecutive errors halt a CPU (6.4 #1)
    u32 tierra_share;   // tierra0 divide: daughter's share of CPU, in 64ths
    // sparks (6.3)
    double spark_rate; // expected sparks per tick (live knob)
    u32 spark_initial; // sparks at tick 0
    u32 spark_grant;   // millicores per spark, from the free pool
    u32 spark_min, spark_max;
    // heat (4.5)
    bool heat_on;
    float cool;       // fraction of heat lost per tick (the node's climate)
    float cpu_gain;   // CPU-unit heat per tick at 100% utilisation
    float fetch_gain; // page heat per instruction fetched from it
    float write_gain; // page heat per codon written into it
    HeatCurve cpu, mem;
    // hazards, baseline rates scaled by the heat error multiplier (6.3, 6.4 #22)
    double copy_rate; // per codon copied (COPY, movii, INJECT)
    double flaw_rate; // per instruction executed
    double rot_rate;  // per codon per tick
    // contact (ENGULF / INJECT, 6.4 #6-7)
    u32 contact_range;      // max distance from the attacker's IP to a surface
    u32 contact_base;       // instructions charged per attempt, success or not
    double contact_per_codon; // instructions charged per codon moved
    // optional seeded genome on every node (smoke tests, demos)
    Array<u8> seed_genome;
    u32 seed_at;
    u32 seed_millicores;
};

// Reads every soup key from cfg (marking them used). On a bad value returns
// false with the reason in cfg.value_error().
bool soup_params_from_config(Config& cfg, u32 soup_size, SoupParams* p);
// Turns a list of Op names into codons using table t (first codon per Op).
bool genome_from_names(const CodonTable& t, Str names, Array<u8>* out, TextBuf* err);

struct Block {
    u32 start, len; // range [start, start + len); codons in it may be lost to heat
    u32 body;       // owner, or kNone for a free slot
    u32 next;       // next block of the same body
    u32 owned;      // codons in the range still owned by this block
    u32 pad;
};

struct Body {
    u64 serial;   // creation order on this node (the cyclic schedule order)
    u32 blocks;   // first block: the body's own code, whose leading NOPs are its surface
    u32 latest;   // most recently allocated block (DIVIDE's daughter), or kNone
    u32 cpus;     // first CPU
    u32 ncpu;
    u32 unit_head, unit_tail, units; // owned millicores (oldest first)
    u32 mem;      // codons owned
    u32 alive;
    float carry;  // fractional instructions (or attack debt) carried to next tick
    u32 work;     // instructions charged this tick (CPU heat input)
    float speed;  // sum of the speed factors of its units, this tick
    float err;    // hazard multiplier from its mean CPU heat, this tick
    u32 pad;
};

struct Cpu {
    u32 ax, bx, cx, dx;
    u32 stack[kMaxStack];
    u32 ip, rh, wh;    // instruction pointer, read head, write head
    u32 body;
    u32 block;         // block the IP last executed in (its wrap range)
    u32 next;          // next CPU of the same body
    u64 executed;      // instructions executed since birth
    u32 sp;
    u8 err;            // error flag: did the last instruction fail
    u8 errs;           // consecutive errors
    u8 halted;
    u8 yielded;        // YIELD: idle for the rest of this tick
};

// Block, Body and Cpu are hashed and snapshotted as raw bytes (Soup::hash_into,
// Soup::save), so they must have no implicit padding: uninitialised padding
// would make the world hash differ between runs. Each size below is the exact
// sum of its fields; if a field is added, add explicit padding to match and
// update the number.
static_assert(sizeof(Block) == 6 * 4, "Block must have no implicit padding");
static_assert(sizeof(Body) == 8 + 14 * 4, "Body must have no implicit padding");
static_assert(sizeof(Cpu) == (4 + kMaxStack + 6) * 4 + 8 + 4 + 4, "Cpu must have no implicit padding");

// Per-tick counters (stats input).
struct SoupTally {
    u64 instructions;
    u64 births;
    u64 deaths;
    u64 sparks;
    u64 halts;
    u64 copy_errors;
    u64 flaws;
    u64 rot_flips;
    u64 cpu_released; // units freed by heat
    u64 mem_released; // codons freed by heat
    u64 engulf_tries, engulfs;
    u64 inject_tries, injects;
};

class Soup {
  public:
    Soup() = default;
    Soup(const Soup&) = delete;
    Soup& operator=(const Soup&) = delete;

    // Random waste everywhere, every unit in the pool, then the seeded genome
    // (if any) and the initial sparks. p must outlive the soup.
    void init(const SoupParams* p, u64 seed, u32 node);
    // The node's whole update for one tick.
    void tick(u64 tick);

    // --- building blocks (also the test API) --------------------------------
    // Starts the RNG stream for (seed, node, tick) and clears the tally.
    void begin_tick(u64 tick);
    // Claims waste [start, start + len) as a new body with one CPU at start and
    // up to `units` millicores from the pool. Returns its slot, or kNone if the
    // range is not all waste.
    u32 create_body(u32 start, u32 len, u32 units);
    // Executes up to `quota` instructions on CPU c; returns how many it charged.
    i64 run_cpu(u32 c, i64 quota);
    void death_sweep();

    // --- inspection ---------------------------------------------------------
    const SoupParams& params() const { return *p_; }
    u32 size() const { return p_->size; }
    const u8* mem() const { return mem_.data(); }
    u8* mem_mut() { return mem_.data(); } // tests: plant code in waste
    u32 owner_block(u32 a) const { return owner_[a]; }
    u32 owner_body(u32 a) const { return owner_[a] == kNone ? kNone : blocks_[owner_[a]].body; }
    usize body_count() const { return order_.size(); } // live bodies, schedule order
    u32 body_at(usize k) const { return order_[k]; }
    const Body& body(u32 b) const { return bodies_[b]; }
    const Cpu& cpu(u32 c) const { return cpus_[c]; }
    Cpu& cpu_mut(u32 c) { return cpus_[c]; }
    const Block& block(u32 k) const { return blocks_[k]; }
    u32 free_units() const { return static_cast<u32>(pool_.size()); }
    u32 waste() const { return waste_; }
    float unit_heat(u32 u) const { return unit_heat_[u]; }
    u32 unit_owner(u32 u) const { return unit_owner_[u]; }
    float page_heat(u32 page) const { return page_heat_[page]; }
    void set_page_heat(u32 page, float h) { page_heat_[page] = h; }
    void set_unit_heat(u32 u, float h) { unit_heat_[u] = h; }
    const SoupTally& tally() const { return tally_; }
    // Checks every bookkeeping invariant (ownership, counts, lists, pool).
    bool check(TextBuf* why) const;
    // Free-run queries: the first p >= from with [p, p + len) all waste, and
    // the largest p with p + len <= to; kNone if there is none.
    u32 find_free_fwd(u32 from, u32 len) const;
    u32 find_free_bwd(u32 to, u32 len) const;

    void hash_into(Hasher& h) const;
    void save(ByteWriter& w) const;
    bool load(ByteReader& r, TextBuf* err);

  private:
    friend struct Exec;

    // ownership and allocation
    u32 new_block(u32 start, u32 len, u32 body);
    void free_block(u32 k);
    // A failed search for a free run of len proves none exists anywhere; that
    // stays true until codons are freed (allocations only shrink free space).
    // A pure cache: it never changes a result, only skips a doomed scan.
    bool known_full(u32 len) const { return fail_epoch_ == free_epoch_ && len >= fail_len_; }
    void note_full(u32 len) {
        if (fail_epoch_ != free_epoch_ || len < fail_len_) {
            fail_epoch_ = free_epoch_;
            fail_len_ = len;
        }
    }
    void mark_used(u32 start, u32 len, bool used);
    bool is_free(u32 start, u32 len) const;
    u32 new_body();
    u32 new_cpu(u32 body, u32 start);
    void kill(u32 b);
    void forget_block(u32 body, u32 k); // CPUs of body whose wrap block is k lose it

    // CPU units
    u32 claim_units(u32 body, u32 k); // random units from the pool
    void move_units(u32 from, u32 to, u32 k); // oldest first
    void release_units(u32 body, u32 k);      // oldest first, into the pool
    void unlink_unit(u32 u);
    void append_unit(u32 body, u32 u);
    void pool_push(u32 u);
    void pool_remove(u32 u);

    // tick phases
    void sparks(u32 count);
    void prepare_heat();
    void run_body(u32 b, u64 tick);
    void heat_update();
    void bit_rot();

    const SoupParams* p_ = nullptr;
    u64 seed_ = 0;
    u32 node_ = 0;
    Rng rng_ = Rng::from_base(0);

    Array<u8> mem_;
    Array<u32> owner_; // block id per codon, kNone = waste
    Array<u64> used_;  // bitmap mirror of owner_ != kNone (fast free-run search)
    u32 waste_ = 0;

    Array<Block> blocks_;
    Array<u32> free_blocks_;
    Array<Body> bodies_;
    Array<u32> free_bodies_;
    Array<Cpu> cpus_;
    Array<u32> free_cpus_;
    Array<u32> order_; // live bodies by serial
    u64 next_serial_ = 0;

    Array<u32> unit_owner_;
    Array<float> unit_heat_;
    Array<u32> unit_next_, unit_prev_;
    Array<u32> pool_;     // free units
    Array<u32> pool_pos_; // index in pool_, or kNone

    Array<float> page_heat_;
    Array<u32> page_fetch_, page_write_; // this tick's heat inputs

    // per-tick working state (not part of the world state)
    SoupTally tally_ = {};
    double flaw_left_ = 0, copy_left_ = 0; // hazard countdowns, redrawn every tick
    Array<u32> scratch_;
    u64 free_epoch_ = 0;         // bumped whenever a codon becomes waste
    u64 fail_epoch_ = ~0ull;     // see known_full()
    u32 fail_len_ = 0;
};

} // namespace nova
