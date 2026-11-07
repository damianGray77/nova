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
// runs instructions in proportion to the speed of the units it owns, less the
// upkeep on the memory it holds (Phase 1b, design.md 6.6), split equally among
// its running CPUs. A body with no running CPU -- none left un-halted, or no
// millicore to run on -- dies in the sweep at the end of the tick (6.4 #1).
//
// Everything a tick does is a function of (seed, node, tick) and the soup's own
// state, so a soup can be updated on any thread (design.md 7.1 D).
#include "nova/accelop.h"
#include "nova/array.h"
#include "nova/config.h"
#include "nova/cost.h"
#include "nova/hash.h"
#include "nova/io.h"
#include "nova/isa.h"
#include "nova/mapped.h"
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

// Selector numbering (design.md 6.3, Phase 1b D6): which variant each BX value
// picks. `nearest` is the identity (0 = the variant nearest to self); the
// shuffled control draws a permutation per run in which the self variant is
// never left at 0. A BX value past the end of a map fails, as before.
inline constexpr u32 kSenseReadings = 8;     // without a GPU or NPU (6.5)
inline constexpr u32 kSenseReadingsGpu = 11; // + my GPU units, their heat, free GPU units (6.7)
inline constexpr u32 kSenseReadingsNpu = 14; // + my NPU units, their heat, free NPU units (6.8)
inline constexpr u32 kSenseReadingsWorld = 15; // + my node's degree, with dispersal (6.9)
inline constexpr u32 kSenseReadingsDisk = 17;  // + my disk codons, free disk codons (6.10)
inline constexpr u32 kSenseReadingsLife = 18;  // + node health, with the lifecycle (6.10)
inline constexpr u32 kSenseReadingsAsync = 20; // + my last accelerator result, my ops pending (6.11)
inline constexpr u32 kSenseReadingsWarp = 22;  // + my lane, lanes in my warp, with warps (6.14)
inline constexpr u32 kSenseReadingsActions = 24; // + ticks until an action on me ends, what it is (6.15)
inline constexpr u32 kSenseReadingsThreads = 28; // + my threads, my kind, my E millicores, free E (9c)
inline constexpr u32 kSenseReadingsMax = 28;
inline constexpr u32 kGpuOps = 16;           // accelerator BX 0-15 run on the GPU (6.8)
inline constexpr u32 kNpuOps = 4;            // BX 16-19 run on the NPU
inline constexpr u32 kLaunchOp = kGpuOps + kNpuOps; // BX 20 launches a warp (Phase 8, 6.14)
inline constexpr u32 kThreadOp = kLaunchOp + 1;     // BX 21 starts a thread (Phase 9c)
inline constexpr u32 kKindOp = kLaunchOp + 2;       // BX 22: this thread prefers P (AX even) or E cores
inline constexpr u8 kKindP = 0, kKindE = 1;          // a thread's preferred kind of core; a millicore's kind
inline constexpr u32 kMaxLanes = 64;         // warp.lanes, at most
inline constexpr u32 kFormCpu = 0;           // a body born on a CPU (Body::form)
inline constexpr u32 kFormGpu = 1;           // ... on a GPU, as a warp (Phase 8)
inline constexpr u32 kMaxSeedLen = 1024;     // longest seeded genome

struct SelectorMap {
    u8 sense[kSenseReadingsMax]; // SENSE: BX -> reading (6.5 list; 8-10 GPU, 11-13 NPU, 14 degree,
                                 // 15-16 disk, 17 node health, 18-19 async accelerator)
    u8 res[7];    // ALLOC and RELEASE: BX -> resource (0 memory, 1 CPU, 2 GPU, 3 disk, 4 NPU,
                  // 5 E millicores: Phase 9c; 1 is then P millicores; 6 archive: docs/world-plan.md W4)
    u8 head[2];   // SETH and GETH: BX -> head (0 read, 1 write)
    u8 divide[4]; // DIVIDE: BX -> destination (0 here, k neighbour edge k)
    u8 gpu[kGpuOps]; // the accelerator verb, BX 0-15 -> GPU op (0 compare ... 15 mul)
    u8 npu[kNpuOps]; // BX 16-19 -> NPU op (0 dot, 1 matvec, 2 matmul, 3 argmax)
    u8 disk[3];   // DISK: BX -> op (0 compare, 1 store, 2 load) (Phase 4)
    u8 sense_n;   // readings SENSE has: up to the highest enabled one (8, 11, 14, 15, 17, 18, 20, 22, 24)
    u8 res_n;     // resources ALLOC knows: 4, 5 with an NPU, 6 with E millicores, 7 with an archive
    u8 shuffled;  // 0 nearest, 1 shuffled
    u8 pad;
};

struct SoupParams {
    // memory and compute
    u32 size;           // codons per node
    u32 millicores;     // CPU units per node
    u32 instr_per_core; // instructions per tick for 1000 millicores at full speed
    // Phase 1b rules (design.md 6.6)
    double upkeep;      // mem.upkeep: instructions per owned codon per tick, at 64 codons
    double upkeep_exp;  // mem.upkeep_exp: 1 linear, 4/3 square-cube
    double cpu_wear;    // cpu.wear: chance per held millicore per tick of failing back to the pool
    bool spark_late;    // spark.timing = last: sparks fire after the bodies have run
    // the in-world GPU (Phase 2, design.md 6.7); gpus = 0 means no GPU at all
    u32 gpus;           // world.gpus: GPU units per node
    u32 gpu_elements;   // gpu.elements: element-ops per unit per tick at full speed
    u32 gpu_setup;      // gpu.setup: CPU instructions per GPU op
    double gpu_cpu_per_element; // gpu.cpu_per_element: CPU instructions per element done
    double gpu_wear;    // gpu.wear: like cpu.wear, for held GPU units
    u32 seed_gpus;      // seed.gpus: GPU units for the seeded genome
    // the in-world NPU (Phase 2b, design.md 6.8); npus = 0 means no NPU at all
    u32 npus;           // world.npus: NPU units per node
    u32 npu_macs;       // npu.macs: multiply-accumulates per unit per tick at full speed
    double npu_cpu_per_mac; // npu.cpu_per_mac: CPU instructions per MAC done
    double npu_wear;    // npu.wear
    u32 seed_npus;      // seed.npus: NPU units for the seeded genome
    // the real devices (Phase 5, design.md 6.11): asynchronous accelerator ops
    bool accel_async;   // accel.mode = async (false: instant, exactly as Phase 4)
    u32 accel_latency;  // accel.latency: results land this many barriers on (1 = the same one)
    u32 gpu_latency, npu_latency; // Phase 9c: each device's (accel.latency; timed tables: its round trip)
    u32 npu_max_matvec; // largest matvec k (64 instant, 256 async)
    u32 npu_max_matmul; // largest matmul k (32 instant, 64 async)
    // instruction set
    CodonTable table;
    SelectorMap sel;
    bool open_membrane; // light Tierra mode: read, search and execute anywhere
    u32 stack_depth;    // 1..kMaxStack
    u32 search_limit;   // template search distance, each way
    u32 max_template;   // longest template / surface considered
    u32 halt_after;     // K consecutive errors halt a CPU (6.4 #1)
    u32 tierra_share;   // tierra0 divide: daughter's share of CPU, in 64ths
    // sparks (6.3); rate, grant and lengths are live knobs (Phase 1b D4)
    double spark_rate; // expected sparks per tick
    u32 spark_initial; // sparks at tick 0
    u32 spark_grant;   // millicores per spark, from the free pool
    u32 spark_min, spark_max; // length range; live changes may cross, so use the lower as min
    // heat (4.5)
    bool heat_on;
    float cool;       // fraction of heat lost per tick (the node's climate)
    float cpu_gain;   // CPU-unit heat per tick at 100% utilisation
    float fetch_gain; // page heat per instruction fetched from it
    float write_gain; // page heat per codon written into it
    float gpu_gain;   // GPU-unit heat per tick at 100% utilisation (runs hot: 9)
    float npu_gain;   // NPU-unit heat per tick at 100% utilisation (runs hot: 9)
    float ambient;    // heat.ambient (Phase 4): node-level leak, x the mean CPU heat input
    HeatCurve cpu, mem, gpu, npu;
    // hazards, baseline rates scaled by the heat error multiplier (6.3, 6.4 #22)
    double copy_rate; // per codon copied (COPY, movii, INJECT)
    double flaw_rate; // per instruction executed
    double rot_rate;  // per codon per tick
    // contact (ENGULF / INJECT, 6.4 #6-7)
    u32 contact_range;      // max distance from the attacker's IP to a surface
    u32 contact_base;       // instructions charged per attempt, success or not
    double contact_per_codon; // instructions charged per codon moved
    // INJECT's own criteria (Phase 1b, Damian; design.md 6.6)
    bool inject_key;        // contact.inject_key: needs a key and a host surface (no 0/0 = 1)
    double inject_room;     // contact.inject_room: host block >= room x payload, else it fails (0 = off)
    // dispersal (Phase 3, design.md 6.9): DIVIDE to a neighbour; SENSE 14
    bool dispersal;
    // the disk tier (Phase 4, design.md 6.10); disk = 0 means no disk at all
    u32 disk;             // world.disk: disk codons per node
    u32 disk_cost_base;   // disk.cost_base: instructions per DISK op
    double disk_cost_per_codon; // disk.cost_per_codon
    double disk_rot;      // disk.rot: chance per disk codon per tick of a bit flip
    // --- disk tiers (docs/world-plan.md W4) ---------------------------------
    // The archive: a second, slow, near-permanent tier, after the fast disk
    // in the same ledger (DiskLedger::fast). archive = 0: none, as before.
    u32 archive;          // world.archive: archive codons per node
    double archive_rot;   // archive.rot: like disk.rot, for archive codons
    double archive_decay; // archive.decay: like disk.decay, for free archive codons
    double archive_slow;  // archive.slow: an archive DISK op's access and codons cost this x the fast disk's
    // Each tier's DISK access and per-codon costs are also divided by its
    // substrate speed, disk_speed and archive_speed (W1, below with the other
    // substrate speeds; set by the world, 1 = as measured).
    // --- end disk tiers -------------------------------------------------------
    // volatility (Phase 4): wipes of whole pages into random codons
    double volatility;    // mem.volatility: chance per page per tick that a wipe starts there
    u32 wipe_pages;       // mem.wipe_pages: a wipe covers 1..this many pages
    // remnants and salvage (Phase 4, Damian): 0 = off
    double salvage_ratio; // salvage.ratio K: free salvage up to K x my genome length
    // the signal channel (Phase 4): 0 channels = EMIT and LISTEN fail
    u32 signal_channels;  // signal.channels
    u32 signal_cost_base; // signal.cost_base: instructions per EMIT
    double signal_cost_per_amp; // signal.cost_per_amp: instructions per unit of |CX|
    double signal_floor;  // signal.floor: the noise floor (a node property: vary.noise)
    // the node lifecycle (Phase 4): endurance 0 = off. A node collapses only
    // when it wears out (docs/dormancy-plan.md D7: the starvation rule is gone).
    double endurance;     // node.endurance: codon writes before the node wears out
    double decline;       // node.decline: hazards x (1 + decline x wear^2) as it wears
    bool map;             // a procedural or landscape world (where snapshots before v3 kept a starvation meter)
    // warps: programs on the GPU (Phase 8, design.md 6.14); off = exactly Phase 7
    bool warps;           // gpu.warps
    u32 warp_lanes;       // warp.lanes: lanes per warp (1..64)
    u32 warp_max;         // warp.max: warps a body may hold
    u32 warp_capacity;    // warp.capacity: warp work per GPU unit per tick at full speed
    u32 warp_step_cost;   // warp.step_cost: work per warp step
    u32 warp_lane_cost;   // warp.lane_cost: work per lane run in a step
    u32 warp_rounds;      // warp.rounds: GPU rounds a tick (warps that waited go again)
    u32 spark_gpu_grant;  // spark.gpu_grant: GPU units per spark
    double spark_gpu_born; // spark.gpu_born: chance a spark holding both starts on the GPU
    // measured costs (Phase 9, design.md 6.15); classic = exactly Phase 8
    CostTable cost;       // cost.table: classic or home_v1
    CostTable ecost;      // Phase 9c: the same table on an E-core (cost.h)
    double plain;       // a plain instruction (an add): 1 (classic) or its picoseconds
    double tick_units;    // what 1,000 millicores give a tick: instr_per_core plain instructions
    u32 engulf_cpt;       // engulf.codons_per_tick: the prey's codons an ENGULF takes a tick (timed)
    u32 inject_cpt;       // inject.codons_per_tick: codons an INJECT writes a tick (timed)
    // threads and cores (Phase 9c, design.md 6.15); off = exactly as before
    bool threads;         // cpu.threads: a thread runs on one core at a time; programs start threads
    u32 slices;           // cpu.slices: a body's threads take turns this many times a tick
    u32 max_threads;      // cpu.max_threads: threads (CPUs) a body may hold for BX 21 to start one
    double e_share;       // cpu.e_share: the share of a node's millicores that are E (9c-b2)
    float e_gain;         // heat.e_gain: E millicores' heat gain, against P's (9c-b2)
    bool kinds;           // threads, or E millicores in the world: threads' kinds are world state
                          // (the world's switch: a site's own e_share may be 0)
    // vents, decay, bites and death (Phase 10, design.md 6.16)
    double mem_decay;     // mem.decay: chance per waste codon per tick of losing one set bit
    double disk_decay;    // disk.decay: the same for free disk codons (fossils)
    u32 vent_spacing;     // vent.spacing: codons per vent (0: no vents)
    u32 vent_rate;        // vent.rate: codons a vent writes a tick
    u32 vent_radius;      // vent.radius: how far from its vent a codon lands, at most
    bool bites;           // engulf.bite: ENGULF bites (else it takes whole prey, as in Phase 9)
    bool threads_end;     // life.threads_end: a halted thread leaves its body
    u32 frozen_ticks;     // life.frozen_ticks: a body that runs nothing this long dies (0: never)
    // Dormancy and drift (docs/dormancy-plan.md): a frozen body with reserves
    // goes dormant instead of dying, drifts from node to node, and wakes where
    // a core of its kind is free.
    bool dormancy;        // life.dormancy
    u32 dormant_mem_loss;  // dormant.mem_loss: codons of its memory reserve lost a tick
    u32 dormant_disk_loss; // dormant.disk_loss: disk codons of its disk reserve lost a tick, once memory is gone
    u32 dormant_dwell;     // dormant.dwell: ticks it rests on a node before moving on
    // --- a varied world (docs/world-plan.md W1, W5) ---------------------------
    // Sparks follow node activity (W5): the rate x (1 - occupancy)^this, the
    // occupancy taken as the tick starts (Soup::occupancy). 0 = by time alone.
    double spark_activity; // spark.activity
    // The node's substrate (W1, W2): how well each resource performs, each
    // the floor, 0.5, 1, 1.5 or 2 (the world sets them per node; 1 = as before).
    // Heat, error rates and wear don't change: a fast unit is a better unit.
    float p_speed, e_speed; // P (E) millicores: a millicore's time a tick (the instructions it runs) x this
    float gpu_speed;        // GPU units: lane throughput (block ops, warp work) and the warps' clock x this
    float npu_speed;        // NPU units: multiply-adds a tick (npu.macs) x this
    // The tiers' speeds divide their costs, so they are doubles: the floor
    // (0.1) as a float is 0.100000001, and a cost divided by it would come
    // out a hair under ten times and truncate one short (6 -> 59, not 60).
    double mem_fast_speed, mem_slow_speed; // the memory tiers (W3, not built yet): their costs / these
    double disk_speed, archive_speed;      // the disk tiers (W4): each tier's DISK access and per-codon costs / these
    // optional seeded genome (smoke tests, demos), on every node or one
    u8 seed_genome[kMaxSeedLen];
    u32 seed_len;        // 0 = no seed
    u32 seed_at;
    u32 seed_millicores;
    u32 seed_form;       // seed.form: kFormCpu or kFormGpu (Phase 8)
};
// SoupParams is plain data, so each node of a world can hold its own copy.
static_assert(__is_trivially_copyable(SoupParams), "SoupParams must stay plain data");

// Reads every soup key from cfg (marking them used). On a bad value returns
// false with the reason in cfg.value_error().
bool soup_params_from_config(Config& cfg, u32 soup_size, SoupParams* p);
// Turns a list of Op names into codons using table t (first codon per Op).
bool genome_from_names(const CodonTable& t, Str names, Array<u8>* out, TextBuf* err);
// "nearest", or "shuffled sense=... res=... head=... divide=..." (each map
// lists the variant every BX value picks, BX = 0 first).
void describe_selectors(const SelectorMap& m, TextBuf& out);

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
    u32 form;     // kFormCpu or kFormGpu: what it was born on (Phase 8; always CPU without warps)
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

// Warps (Phase 8, design.md 6.14): a body's code run in lockstep on many
// lanes, each lane a full CPU state. All 32-bit words, because the GPU reads
// and writes this very layout (warp_isa.inc). Heads are offset by lane and
// move by the lane count; see warp_isa.inc for the rules.
struct Lane {
    u32 ax, bx, cx, dx;
    u32 stack[kMaxStack];
    u32 ip, rh, wh;
    u32 block;   // block the IP last executed in, or kNone
    u32 sp;
    u32 err;     // did the last instruction fail
    u32 errs;    // consecutive errors
    u32 halted;
    u32 yielded; // YIELD: idle for the rest of this tick
};
static_assert(sizeof(Lane) == (4 + kMaxStack + 9) * 4, "Lane must be 32-bit words only");

struct Warp {
    u32 body;     // owner, or kNone for a free slot
    u32 next;     // next warp of the same body
    u32 wait;     // stopped at a host instruction: the host runs it after the GPU phase
    u32 wait_ip;
    u64 steps;    // steps run since launch
    u64 lane_instr; // lane instructions run since launch
};
static_assert(sizeof(Warp) == 4 * 4 + 16, "Warp must have no implicit padding");

// An action in progress (Phase 9, design.md 6.15): with a timed cost table,
// ENGULF and INJECT, and any instruction longer than a tick, take ticks. It
// starts at its actor's turn (checked, its target marked); its CPU then waits,
// running nothing; at its end, at the actor's turn, it is checked again and
// lands or fails. World state (hashed, snapshotted) with a timed table.
struct Action {
    u64 end;           // the tick it ends: it resolves at its actor's turn then
    u64 actor_serial;  // the actor body's serial (it may die or be eaten meanwhile)
    u64 target_serial; // the target body's serial (contact), else 0
    u32 op;            // the instruction (Op): ENGULF, INJECT or DISK
    u32 cpu;           // the actor's CPU slot
    u32 actor;         // the actor's body slot
    u32 target;        // the target's body slot (contact), or kNone
    u32 n;             // the aim: its template's length
    u32 match, len;    // the aim's surface match at the start (its chance: match / len)
    u32 warp;          // 1: the actor is a warp (Phase 9b), and `cpu` is its slot
    u32 bite;          // Phase 10, ENGULF as a bite: the most it takes, codons
    u32 at;            // ... and, biting raw material (target kNone), the waste codon it aims at
    u8 bits[64];       // the aim's template bits
};
static_assert(sizeof(Action) == 3 * 8 + 10 * 4 + 64, "Action must have no implicit padding");
// Whether an action is contact (the CPU at work: ENGULF, INJECT) rather than
// a wait (a long instruction). A bite of raw material has no target body.
inline bool contact_action(const Action& a) {
    return a.op == static_cast<u32>(Op::engulf) || a.op == static_cast<u32>(Op::inject);
}
inline constexpr u32 kActEngulf = 1; // SENSE 23's codes for what is acting on me
inline constexpr u32 kActInject = 2;

// A pool of owned, heated compute units held per body: the in-world GPU and
// NPU (design.md 6.7, 6.8). The same ledger as millicores -- claims at random
// from the pool, release and transfers oldest first, freed units keep their
// heat -- with the per-body lists kept here rather than in Body (which is
// hashed raw and must not change shape). `per_unit` is the work one unit does
// per tick at full speed (GPU element-ops, NPU multiply-accumulates).
struct UnitLedger {
    Array<u32> owner, next, prev, pool, pool_pos;
    Array<float> heat;
    Array<u32> head, tail, count; // per body slot
    // per-tick working state (not world state)
    Array<float> cap; // work left this tick, per body
    Array<u32> done;  // work done this tick, per body (the heat input)
    Array<float> err; // copy-error multiplier from mean heat, per body

    u32 units() const { return static_cast<u32>(owner.size()); }
    void init(u32 n);
    void body_slot(u32 body, usize nbodies); // sizes the per-body arrays, clears slot body
    void append(u32 body, u32 u);
    void unlink(u32 u);
    void pool_push(u32 u);
    void pool_remove(u32 u);
    u32 claim(Rng& rng, u32 body, u32 k);    // random units from the pool; returns how many
    void move(u32 from, u32 to, u32 k);      // oldest first
    void release(u32 body, u32 k);           // oldest first, into the pool
    void prepare(const Array<u32>& order, const HeatCurve& curve, bool heat_on, float per_unit);
    // Returns how many were freed; `ambient` is the node's leak this tick.
    u32 heat_update(const HeatCurve& curve, float gain, float decay, float per_unit, float ambient);
    u32 wear(Rng& rng, double f);            // returns worn
    void hash_into(Hasher& h) const;
    void save(ByteWriter& w) const;
    bool load(ByteReader& r, u64 nbodies, TextBuf* err);
    // Every invariant; `what` names the unit kind in the message.
    bool check(const Array<Body>& bodies, const char* what, TextBuf* why) const;
};

// First p >= from with [p, p + len) all clear in a bitmap of n positions, or
// kNone (memory's free-run search, shared with the disk).
u32 free_run_fwd(const u64* used, u32 n, u32 from, u32 len);

// The disk tier (Phase 4, design.md 6.10): a second codon array per node,
// owned in extents (contiguous runs) that each body keeps in claim order.
// A body's disk is addressed relative to its own holdings: offset 0 is the
// start of its first extent, and its extents follow one another. Moving disk
// between bodies relabels extents, splitting one at most. Freed disk keeps
// its contents (fossils), as memory waste does.
struct DiskExtent {
    u32 start, len;
    u32 body; // owner, or kNone for a free slot
    u32 next; // next extent of the same body
};
static_assert(sizeof(DiskExtent) == 16, "DiskExtent must have no implicit padding");

struct DiskLedger {
    // The per-codon arrays live in RAM, or in files once bound (Phase 7b:
    // disk.path puts the disk tier on real disk; only the storage changes).
    Backed<u8> mem;
    Backed<u32> stamp;     // salvage stamps per codon (empty when the rule is off)
    Backed<u32> owner;     // extent id per codon, kNone = free
    Backed<u64> used;      // bitmap mirror of owner != kNone
    // Binds the per-codon arrays to files in `folder` (n<node>.mem and so on);
    // false if they can't be created.
    bool bind_files(const char* folder, u32 node);
    Array<DiskExtent> ext;
    Array<u32> free_ext;   // free extent slots
    Array<u32> head, tail, total; // per body slot
    u32 free = 0;          // free codons
    // Disk tiers (docs/world-plan.md W4): codons [fast, size) are the
    // archive, so an extent's tier is where it lies, and none straddles the
    // boundary. fast == size: no archive (and everything below as before).
    u32 fast = 0;          // the archive's first codon
    u32 afree = 0;         // free archive codons (counted in `free` too)

    u32 size() const { return static_cast<u32>(mem.size()); }
    void init(u32 n, Rng& rng, u32 fast_n); // random contents, all free; [fast_n, n) the archive
    void body_slot(u32 body, usize nbodies);
    // One run of len free codons, first fit from `from`, then from 0; the
    // offset (in the body's disk) of the new extent, or kNone. W4: within
    // the archive (arch) or the fast disk.
    u32 alloc_run(Rng& rng, u32 body, u32 len, bool arch = false);
    // Up to `count` free codons in as many runs as it takes, from a random
    // start, wrapping once; returns how many were claimed. W4: within one tier.
    u32 claim_any(Rng& rng, u32 body, u32 count, bool arch = false);
    // Frees the newest k codons; with stamps, those with stamp 0 take `stamp`.
    void release_tail(u32 body, u32 k, u32 stamp = 0);
    void move_tail(u32 from, u32 to, u32 k); // the newest k codons change hands
    void move_all(u32 from, u32 to);
    // The absolute addresses of [off, off + n) of the body's disk, or false.
    bool addresses(u32 body, u32 off, u32 n, Array<u32>* out) const;
    u32 rot(Rng& rng, double rate);        // returns flips
    u32 decay(Rng& rng, double rate);      // Phase 10: free codons lose bits; returns bits lost
    // --- disk tiers (W4) -----------------------------------------------------
    bool tiered() const { return fast < size(); }
    u32 fast_free() const { return free - afree; }
    u32 held_in(u32 body, bool arch) const; // the body's codons in one tier
    // The archive codons among [off, off + n) of the body's disk; false if
    // the range isn't all held.
    bool archive_in(u32 body, u32 off, u32 n, u32* arch) const;
    // The newest kf fast and ka archive codons, freed (with stamps as
    // release_tail) or handed to another body, keeping their claim order.
    void release_tiers(u32 body, u32 kf, u32 ka, u32 stamp = 0);
    void move_tiers(u32 from, u32 to, u32 kf, u32 ka);
    // rot and decay over [lo, hi) only (the old ones: the whole ledger).
    u32 rot_range(Rng& rng, double rate, u32 lo, u32 hi);
    u32 decay_range(Rng& rng, double rate, u32 lo, u32 hi);
    // --- end disk tiers -------------------------------------------------------
    void hash_into(Hasher& h) const;
    void save(ByteWriter& w) const;
    bool load(ByteReader& r, u64 nbodies, TextBuf* err);
    bool check(const Array<Body>& bodies, TextBuf* why) const;

  private:
    u32 find_run(u32 from, u32 len, u32 lo, u32 hi) const;
    u32 add_extent(u32 body, u32 start, u32 len);
    u32 split_tail(u32 body, u32 k); // detaches the newest k codons; returns the chain head
    u32 split_tiers(u32 body, u32 kf, u32 ka); // W4: the same, per tier
    u32 new_slot();
    void free_chain(u32 x, u32 stamp);   // frees a detached chain (fossils)
    void append_chain(u32 to, u32 x);    // gives a detached chain to body `to`
};

// What a collapsed node leaves for its next life (Phase 4 lifecycle): its
// memory and disk with their stamps, and how long it lay fallow.
struct Ruins {
    Array<u8> mem, disk;
    Array<u32> mem_stamp, disk_stamp;
    u64 fallow = 0;
    u32 disk_fast = 0; // disk tiers (W4): where its archive began in `disk`
};

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
    u64 upkeep;   // instructions charged as memory upkeep (Phase 1b D1)
    u64 cpu_worn; // millicores returned to the pool by CPU wear (Phase 1b D10)
    u64 gpu_elements; // GPU element-ops done (Phase 2)
    u64 gpu_released; // GPU units freed by heat
    u64 gpu_worn;     // GPU units returned by wear
    u64 npu_macs;     // NPU multiply-accumulates done (Phase 2b)
    u64 npu_released; // NPU units freed by heat
    u64 npu_worn;     // NPU units returned by wear
    u64 sent;         // daughters dispersed to a neighbour (Phase 3)
    u64 arrived;      // daughters placed here on arrival
    u64 lost;         // arrivals with no room here, lost
    // Phase 4 (design.md 6.10); the world adds the ones marked (world)
    u64 corrupted;    // codons flipped in transit (world)
    u64 transit_lost; // cargoes lost on a failed or vanished link (world)
    u64 disk_stores, disk_loads, disk_compares; // DISK ops done
    u64 disk_rot;     // disk codons flipped by rot
    u64 wipes;        // volatility wipes
    u64 wiped;        // codons wiped
    u64 wipe_deaths;  // bodies killed by a whole-body wipe
    u64 emits, listens; // signal ops done
    u64 collapses;    // nodes collapsed (world)
    u64 recoveries;   // nodes appeared or recovered (world)
    u64 garbled;      // codons the salvage rule replaced with random ones
    // Phase 8 warps (design.md 6.14)
    u64 warp_steps;   // warp steps run
    u64 lane_instr;   // lane instructions run (a step runs one per lane in it)
    u64 warp_host;    // host instructions done for warps
    u64 launches;     // warps launched (GPU verb BX 20)
    u64 gpu_births;   // bodies born on a GPU
    // Phase 9 actions in progress (design.md 6.15; timed cost tables)
    u64 actions_started;  // ENGULFs, INJECTs and long instructions begun
    u64 actions_landed;   // ... that landed at their end
    u64 actions_failed;   // ... that failed at their end (or lost their actor)
    u64 actions_defended; // ... of those, contact whose target changed in time (its surface, or out of range)
    // Phase 9c threads and cores (design.md 6.15)
    u64 threads_started;  // threads started by programs (GPU verb BX 21)
    u64 e_work;           // work done on E millicores (instructions, or world time with a timed table)
    // Phase 10: vents, decay, bites and death (design.md 6.16)
    u64 vent_codons;      // codons vents wrote into waste
    u64 decayed;          // bits decayed out of dead matter (memory waste and disk fossils)
    u64 bites_waste;      // ENGULF bites of raw material
    u64 bites_body;       // ENGULF bites that took a piece of a bigger body (whole swallows: engulfs)
    u64 codons_bitten;    // codons taken by bites, whole swallows included
    u64 threads_ended;    // halted threads that left their bodies
    u64 died_halted;      // bodies dead with no running thread
    u64 died_frozen;      // bodies dead after life.frozen_ticks without running an instruction
    u64 died_worn;        // bodies dead with no millicore left (wear, heat, bites)
    // Dormancy and drift (docs/dormancy-plan.md); the world adds the ones marked (world)
    u64 dormant_started;  // frozen bodies that went dormant instead of dying
    u64 dormant_woke;     // dormant programs that woke where they rested
    u64 dormant_expired;  // ... that died with their reserves spent (some in transit: world)
    u64 dormant_eaten;    // ... swallowed whole while resting
    u64 dormant_hops;     // hops taken by drifting programs (passing through: world)
    // Disk tiers (docs/world-plan.md W4)
    u64 archive_stores;   // DISK stores that touched the archive (counted in disk_stores too)
    u64 archive_loads;    // DISK loads that touched the archive (counted in disk_loads too)
};

class Lineages; // lineage.h: the replicator detector, a pure observer

// What a dispersing daughter's lineage tag carries (Phase 3): the detector
// adopts her on arrival, so lineages span nodes.
struct CargoTag {
    u64 lineage;  // world-unique lineage id, 0 = not a member
    u64 genotype;
    u64 parent;   // her genotype's first parent, for the genebank
    u32 depth;
    u32 gated;    // her lineage had met the gate where she left
};

// A daughter leaving this node this tick (moved into transit at the barrier).
struct Departure {
    CargoTag tag;
    u32 nbr;       // neighbour number - 1 (an index into the node's ports)
    u32 len, codon_off;
    u32 millicores, gpus, npus; // the share she draws on arrival
    u32 disk;      // disk codons she claims on arrival (Phase 4)
    u32 form;      // kFormCpu or kFormGpu: what she is born on there (Phase 8)
    u32 emillicores; // her share of E millicores (Phase 9c; millicores is then P)
    u32 kind;        // the kind of core her thread prefers: her divider's (Phase 9c)
    // A dormant program drifting on (docs/dormancy-plan.md): the cores and disk
    // above are what it held at home, claimed up to when it wakes; it travels
    // outside the edge's capacity.
    u32 dormant;     // 1: a dormant program, 0: a daughter
    u32 mem_reserve; // its memory reserve (codons beyond its code)
    u32 hops;        // hops so far, this one included
    // Disk tiers (docs/world-plan.md W4): her archive codons claimed on
    // arrival (`disk` is then her fast disk's); a dormant program's archive
    // reserve.
    u32 archive;
};

// A dormant program resting on this node (docs/dormancy-plan.md): a body with
// its code and nothing else, which doesn't run. Raw-hashable, per body slot.
struct Dormant {
    u32 on;           // 1 while dormant
    u32 dwell;        // ticks left before it moves on
    u32 mem_reserve;  // its memory beyond its code, spent first by the clock
    u32 disk_reserve; // its disk holding, spent once memory is gone (W4: its fast disk's)
    u32 millicores, emillicores, gpus, npus; // what it held at home (P, E, GPU, NPU)
    u32 form;         // kFormCpu or kFormGpu
    u32 kind;         // its thread's preferred kind of core
    u32 hops;         // hops it has drifted
    u32 archive_reserve; // disk tiers (W4): its archive holding, spent last (was padding: 0 without one)
};
static_assert(sizeof(Dormant) == 48, "Dormant must have no implicit padding");

// A node's view of its out-edges, set by the world (Phase 3): edge ids nearest
// neighbour first, how many cargoes each already has in flight (world state,
// changed only at the barrier), and the edge capacity. Phase 4: which edges
// are down (link failures), or nullptr when links never fail.
struct DispersalPorts {
    const u32* edges = nullptr;
    const u32* in_flight = nullptr; // indexed by edge id
    const u8* down = nullptr;       // indexed by edge id
    u32 degree = 0;
    u32 capacity = 0;
};

// Debugging (warp.verify, warp.cpp): while on, the reference logs every
// instruction of one lane (the warp whose first lane is lane0, its lane
// `lane`) into `out`, a line each. One thread at a time.
void warp_trace_begin(u32 lane0, u32 lane, TextBuf* out);
void warp_trace_end();

// The CPU reference on one body record of a packed round (warp.cpp): img is
// the round's image, cst its constants, lanes the node's lanes by slot. For
// `nova bench warp`, which replays a dumped round outside any soup.
void warp_run_reference(u32* img, const u32* cst, Lane* lanes, u32 rec);
// While set (nullptr: off), the reference counts per Op, into counts[0 ..
// 2 x kOpCount + 1): lane-instructions (kOpCount: a lane that couldn't run
// the code), then places searched. One thread at a time (`nova bench warp --ops`).
void warp_op_histogram(u64* counts);

class Soup {
  public:
    Soup() = default;
    Soup(const Soup&) = delete;
    Soup& operator=(const Soup&) = delete;

    // Phase 7b: put the disk tier in files in this folder (which must outlive
    // the soup); before init.
    void set_disk_folder(const char* folder) { disk_folder_ = folder; }

    // Random waste everywhere, every unit in the pool, then the seeded genome
    // (if any) and the initial sparks. p must outlive the soup. Phase 4: a
    // later life (life > 0) draws its random soup from its own stream, and
    // `ruins` (if given) lie over it first, weathered by rot for their fallow
    // time, where the sizes overlap.
    void init(const SoupParams* p, u64 seed, u32 node, u32 life = 0, const Ruins* ruins = nullptr);
    // The node's whole update for one tick.
    void tick(u64 tick);

    // --- building blocks (also the test API) --------------------------------
    // Starts the RNG stream for (seed, node, tick) and clears the tally.
    void begin_tick(u64 tick);
    // Claims waste [start, start + len) as a new body with one CPU at start and
    // up to `units` millicores from the pool. Returns its slot, or kNone if the
    // range is not all waste. (Phase 9c: with E millicores, drawn from both
    // pools in proportion to what each holds free, as a spark draws.)
    u32 create_body(u32 start, u32 len, u32 units);
    // Executes up to `quota` instructions on CPU c; returns how many it charged.
    // Phase 9c: on a core of `kind`, at that kind's costs (cost.h).
    i64 run_cpu(u32 c, i64 quota, u8 kind = kKindP);
    // Phase 9c: up to k P (E) millicores at random from that pool; returns how
    // many. ALLOC's CPU (E) selector.
    u32 claim_p_units(u32 body, u32 k) { return claim_units(body, k); }
    u32 claim_e_units(u32 body, u32 k);
    void death_sweep();
    // Gives body up to k GPU (NPU) units chosen at random from the pool;
    // returns how many (0 without one). Also what ALLOC's selector does.
    u32 claim_gpus(u32 body, u32 k);
    u32 claim_npus(u32 body, u32 k);
    // Sets every body's GPU and NPU capacity for this tick from its units'
    // heat (the tick does this itself; tests driving run_cpu directly call it).
    void prepare_gpu();
    // The lifecycle (Phase 4). collapse_due(): worn out, decided at the end of
    // the tick. collapse(): every body dies an ordinary death (its
    // code stays behind as remnants). idle(): a fallow node's tick (nothing
    // runs). take_ruins(): its memory and disk, for its next life.
    bool collapse_due() const { return collapse_; }
    void collapse();
    void idle(u64 tick) { begin_tick(tick); }
    void take_ruins(Ruins* out, u64 fallow) const;
    bool lifecycle() const { return p_->endurance > 0; }
    u64 writes() const { return writes_; }
    // Diagnostics (not hashed, not in snapshots): arrivals that found no free
    // millicore here, so died in their first sweep.
    u64 arrived_dry() const { return arrived_dry_; }
    u64 arrived_all() const { return arrived_all_; } // ... and every arrival, likewise
    u32 health() const; // 0-100, the SENSE reading: what's left of the node's write endurance

    // Dormancy and drift (docs/dormancy-plan.md). A dormant program arriving
    // here (at the barrier, in the fixed delivery order) lands in a waste run
    // its code fits, as a resting body that doesn't run; false (changing
    // nothing) when there is no room, and the world sends it on. Its lineage
    // tag waits with it, for when it wakes.
    bool land_dormant(const u8* code, u32 len, const Dormant& d, const CargoTag& tag);
    bool dormant(u32 b) const { return b < dorm_.size() && dorm_[b].on != 0; }
    bool force_dormant(u32 b) { return go_dormant(b); } // tests: as if the watchdog found it frozen
    const Dormant& dormant_state(u32 b) const { return dorm_[b]; }
    u32 resting_count() const; // dormant programs resting here
    // A varied world (docs/world-plan.md W5): the share of memory held by
    // living bodies, counting neither waste nor resting dormant programs; and
    // the spark rate it gives now, spark.rate x the node's spark weight x
    // (1 - occupancy)^spark.activity. A tick's sparks are drawn at its start,
    // from the occupancy then (after the barrier's arrivals and landings).
    double occupancy() const;
    double spark_rate_now() const;
    // Diagnostics (not hashed, not in snapshots): frozen bodies that died with
    // no reserves to go dormant on, and the reserves of those that went.
    u64 frozen_bare() const { return frozen_bare_; }
    u64 dormant_mem_sum() const { return dormant_mem_sum_; }
    u64 dormant_disk_sum() const { return dormant_disk_sum_; }

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
    u32 free_units() const { return static_cast<u32>(pool_.size() + epool_.size()); } // both kinds (9c)
    u32 waste() const { return waste_; }
    float unit_heat(u32 u) const { return unit_heat_[u]; }
    u32 unit_owner(u32 u) const { return unit_owner_[u]; }
    float page_heat(u32 page) const { return page_heat_[page]; }
    void set_page_heat(u32 page, float h) { page_heat_[page] = h; }
    void set_unit_heat(u32 u, float h) { unit_heat_[u] = h; }
    // GPU (Phase 2) and NPU (Phase 2b); all zero / kNone without one
    bool has_gpu() const { return p_->gpus > 0; }
    u32 free_gpus() const { return static_cast<u32>(gpu_.pool.size()); }
    u32 gpu_count(u32 b) const { return b < gpu_.count.size() ? gpu_.count[b] : 0; }
    u32 gpu_owner(u32 u) const { return gpu_.owner[u]; }
    float gpu_heat(u32 u) const { return gpu_.heat[u]; }
    void set_gpu_heat(u32 u, float h) { gpu_.heat[u] = h; }
    float gpu_capacity(u32 b) const { return b < gpu_.cap.size() ? gpu_.cap[b] : 0.0f; }
    bool has_npu() const { return p_->npus > 0; }
    u32 free_npus() const { return static_cast<u32>(npu_.pool.size()); }
    u32 npu_count(u32 b) const { return b < npu_.count.size() ? npu_.count[b] : 0; }
    u32 npu_owner(u32 u) const { return npu_.owner[u]; }
    float npu_heat(u32 u) const { return npu_.heat[u]; }
    void set_npu_heat(u32 u, float h) { npu_.heat[u] = h; }
    float npu_capacity(u32 b) const { return b < npu_.cap.size() ? npu_.cap[b] : 0.0f; }
    const SoupTally& tally() const { return tally_; }
    // The ambient heat input this tick (Phase 4; 0 when heat.ambient is off).
    float ambient() const { return ambient_; }
    // Disk (Phase 4): free disk codons and a body's disk codons (0 without a
    // disk), and the ledger itself.
    // W4: free_disk() is the fast disk's; disk_held() a body's whole disk,
    // both tiers.
    u32 free_disk() const { return disk_on() ? disk_.fast_free() : 0; }
    u32 disk_held(u32 b) const { return b < disk_.total.size() ? disk_.total[b] : 0; }
    const DiskLedger& disk() const { return disk_; }
    DiskLedger& disk_mut() { return disk_; } // tests
    // --- disk tiers (docs/world-plan.md W4) ----------------------------------
    // Whether the node has a disk ledger (a fast disk, an archive or both);
    // free archive codons; a body's archive codons.
    bool disk_on() const { return p_->disk > 0 || p_->archive > 0; }
    u32 free_archive() const { return disk_on() ? disk_.afree : 0; }
    u32 archive_held(u32 b) const { return disk_.tiered() && b < disk_.total.size() ? disk_.held_in(b, true) : 0; }
    // Disk failure on one tier (docs/disasters-plan.md X5, amended by W4):
    // every codon of the fast disk (archive false) or the archive becomes
    // INERT (codon 0), fossils included, stamps cleared; every holding on it
    // is released and resting dormant programs here lose that tier's
    // reserve. Nobody dies directly. Returns the codons lost.
    u32 fail_disk_tier(bool archive);
    // --- end disk tiers -------------------------------------------------------
    // Salvage (Phase 4): a body's genome length (its first block), and the
    // stamp on memory codon a (0 without the rule).
    u32 genome_len(u32 b) const {
        const u32 k = bodies_[b].blocks;
        return k == kNone ? 0 : blocks_[k].len;
    }
    u32 stamp(u32 a) const { return a < stamp_.size() ? stamp_[a] : 0; }
    // Asynchronous accelerator ops (Phase 5, design.md 6.11). The world takes
    // this tick's issued ops at the barrier (gather: appends them to ops,
    // their input codons to in, their write-back addresses to addr) and later
    // hands each back with its results (complete).
    void accel_gather(Array<AccelOp>* ops, Array<u8>* in, Array<u32>* addr);
    void accel_complete(const AccelOp& op, const u8* out, u32 scalar, const u32* addr);
    usize accel_issued() const { return issued_.size(); }
    u32 accel_result(u32 c) const { return c < cpu_result_.size() ? cpu_result_[c] : 0; }
    u32 accel_pending(u32 c) const { return c < cpu_pending_.size() ? cpu_pending_[c] : 0; }

    // Signals (Phase 4): this tick's emission sums per channel (the world
    // spreads them at the barrier), the field heard this tick (the world sets
    // it before the tick), and per-channel counts for the researcher's tap.
    const i64* emissions() const { return emit_.data(); }
    i32* field_mut() { return field_.data(); }
    const i32* field() const { return field_.data(); }
    const u64* emit_counts() const { return emit_count_.data(); }
    const u64* emit_amps() const { return emit_amp_.data(); }
    // Checks every bookkeeping invariant (ownership, counts, lists, pool).
    bool check(TextBuf* why) const;
    // Free-run queries: the first p >= from with [p, p + len) all waste, and
    // the largest p with p + len <= to; kNone if there is none.
    u32 find_free_fwd(u32 from, u32 len) const;
    u32 find_free_bwd(u32 to, u32 len) const;

    void hash_into(Hasher& h) const;
    void save(ByteWriter& w) const;
    // `version`: the snapshot format's (v1 lacks the pages' heat inputs).
    bool load(ByteReader& r, TextBuf* err, u32 version = 2);
    // Debugging: the state a section a file, <prefix>.<section> (soup.cpp).
    void debug_dump(const char* prefix) const;

    // The replicator detector (lineage.h), or nullptr. It only watches: the
    // soup tells it about new blocks, freed blocks and DIVIDEs, and nothing it
    // does feeds back into the soup or its RNG, so the world hash is the same
    // with or without it. Not part of the world state.
    void set_observer(Lineages* obs);
    Lineages* observer() const { return obs_; }
    u64 current_tick() const { return tick_; }
    usize block_slots() const { return blocks_.size(); }

    // Dispersal (Phase 3, design.md 6.9). The world sets the ports; DIVIDE to
    // neighbour k queues a departure; at the barrier the world takes the
    // departures into transit and delivers arrivals with arrive().
    void set_ports(const DispersalPorts& ports) { ports_ = ports; }
    u32 degree() const { return ports_.degree; }
    const Array<Departure>& departures() const { return departures_; }
    const u8* departure_codons() const { return departure_codons_.data(); }
    void clear_departures();
    // A daughter arriving (at the barrier, after tick `tick`): placed on a
    // random waste run of her length, drawing her shares from the pools
    // (capped), with one CPU at her start. False (and counted lost) if there
    // is no room.
    // Phase 4: she also claims `disk` codons of free disk (capped).
    // Phase 8: with warps she is born in `form` (a warp for kFormGpu), or in
    // the other form if she holds only the other's units.
    // Phase 9c: with E millicores, `millicores` are P and `emillicores` E;
    // her thread prefers `kind` (her divider's).
    // W4: `disk` is fast disk; she also claims `archive` codons of free
    // archive (capped), after it.
    bool arrive(const u8* code, u32 len, u32 millicores, u32 gpus, u32 npus, u32 disk, const CargoTag& tag,
                u32 form = kFormCpu, u32 emillicores = 0, u8 kind = kKindP, u32 archive = 0);

    // --- warps (Phase 8, design.md 6.14) ------------------------------------
    // The tick in parts, so that the world can run every node's warps on the
    // GPU in between: tick() is tick_first(), the warp rounds, tick_second().
    // A round: warp_pack() (false: nothing to run), the run (warp_run_ref()
    // here, or the GPU on the packed image), warp_unpack(), warp_host().
    void tick_first(u64 tick);
    bool warp_pack(u32 round);
    void warp_run_ref();
    // all_on_gpu: every record ran on the GPU; otherwise those marked in
    // warp_record_on_gpu() did. Their warps' lanes are now the GPU's.
    void warp_unpack(bool all_on_gpu = false);
    void warp_host(u32 round);
    void tick_second();
    // Where a warp's lanes are current (Phase 8b): the GPU keeps them between
    // rounds, so the host's copy may be stale. kLanesHost: the host's copy is
    // the one (the GPU's must be sent before it runs them); kLanesDevice: the
    // GPU's is (fetch before the host reads them); kLanesBoth: they agree.
    // The soup reads lanes only where they are the host's (host instructions,
    // hashing and the like); the world fetches them first.
    static constexpr u8 kLanesHost = 0, kLanesDevice = 1, kLanesBoth = 2;
    u8 lane_state(u32 w) const { return wstate_[w]; }
    void set_lane_state(u32 w, u8 s) { wstate_[w] = s; }
    bool lanes_on_device() const; // any warp's lanes only on the GPU
    // The round's records after warp_pack, and one run on the CPU reference:
    // records are independent, so threads may share a node's round (the
    // world spreads a heavy node's records over them). warp_run_ref() runs
    // them all, one after another.
    usize warp_record_count() const { return wrec_.size(); }
    void warp_run_record(usize i);
    // The per-body split (Phase 10 profiling): which of the round's records
    // the GPU runs (1; all 0 after warp_pack), each record's size in words,
    // and the length of its body's last first round in warp steps (a GPU
    // runs a body's steps one after another; 0 for a body that hasn't run;
    // the peak, decaying a quarter a round).
    Array<u8>& warp_record_on_gpu() { return wrec_gpu_; }
    u32 warp_record_words(usize i) const {
        return static_cast<u32>((i + 1 < wrec_.size() ? wrec_[i + 1] : wimg_.size()) - wrec_[i]);
    }
    u32 warp_record_chain(usize i) const { return wbody_[i] < wbody_chain_.size() ? wbody_chain_[wbody_[i]] : 0; }
    // Lanes not halted (and not yielded either) after a warp's last run.
    u32 warp_live(u32 w) const { return wlive_[w]; }
    // The longest body's warp steps in the last round (a GPU runs a body's
    // steps one after another, so this is its round's length).
    u32 warp_longest() const { return wchain_; }
    // The round's packed image (body records), the record offsets, the
    // constants, and the node's lanes: what the GPU backend moves.
    Array<u32>& warp_image() { return wimg_; }
    const Array<u32>& warp_image_view() const { return wimg_; }
    const Array<u32>& warp_records() const { return wrec_; }
    const Array<u32>& warp_consts() const { return wcst_; }
    Array<Lane>& lanes_mut() { return lanes_; }
    // Inspection.
    bool warps_on() const { return p_->warps; }
    u32 lanes_per_warp() const { return p_->warp_lanes; }
    usize warp_slots() const { return warps_.size(); }
    u32 warps_alive() const { return static_cast<u32>(warps_.size() - free_warps_.size()); }
    u32 warp_count(u32 b) const { return b < warp_count_.size() ? warp_count_[b] : 0; }
    u32 warp_head(u32 b) const { return b < warp_head_.size() ? warp_head_[b] : kNone; }
    const Warp& warp(u32 w) const { return warps_[w]; }
    const Lane& lane(u32 w, u32 l) const { return lanes_[static_cast<usize>(w) * p_->warp_lanes + l]; }
    Lane& lane_mut(u32 w, u32 l) { return lanes_[static_cast<usize>(w) * p_->warp_lanes + l]; }
    float warp_carry(u32 b) const { return b < warp_carry_.size() ? warp_carry_[b] : 0.0f; }
    // Tests: a warp for body at ip, lanes zeroed, heads offset by lane; kNone
    // if the body already holds warp.max.
    u32 launch_warp(u32 body, u32 ip);
    // Tests and the seed: a body on [start, start + len) born in `form` (with
    // units and GPU units from the pools), or kNone if the range isn't waste.
    u32 create_body_form(u32 start, u32 len, u32 units, u32 gpus, u32 form);

    // --- actions in progress (Phase 9, design.md 6.15) ---------------------
    // With a timed cost table: ENGULFs, INJECTs and long instructions under
    // way, in the order they started. Inspection and the viewer.
    const Array<Action>& actions() const { return actions_; }
    // Whether an action's actor is still the body that started it.
    bool action_live(const Action& a) const;
    // The soonest-ending live action aimed at body b (its index), or kNone.
    u32 action_on(u32 b) const;
    // CPU c's action under way (its index), or kNone.
    u32 action_of(u32 c) const;
    // Phase 9c: the kind of core CPU c's thread prefers (kKindP without threads or E millicores).
    u8 cpu_kind(u32 c) const { return c < cpu_kind_.size() ? cpu_kind_[c] : kKindP; }
    // Phase 9c: CPU c is waiting on the disk or a device (idle, not working).
    bool cpu_waits(u32 c) const;

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
    u32 claim_units(u32 body, u32 k); // random units from the (P) pool
    void move_units(u32 from, u32 to, u32 k); // oldest first
    void release_units(u32 body, u32 k);      // oldest first, into the pool
    // Phase 9c: up to k from both pools in proportion to what each holds free
    // (a spark's draw; just claim_units without E millicores).
    u32 claim_mixed(u32 body, u32 k);
    // Phase 9c: the oldest k units of one kind (E: e true) to another body, or
    // into their pool. Without E millicores, move_units and release_units.
    void move_kind(u32 from, u32 to, u32 k, bool e);
    void release_kind(u32 body, u32 k, bool e);
    void unlink_unit(u32 u);
    void append_unit(u32 body, u32 u);
    void pool_push(u32 u);
    void pool_remove(u32 u);

    // tick phases
    void tick_second_parts(); // tick_second's work (it times it)
    void sparks(u32 count);
    void prepare_heat();
    void run_body(u32 b, u64 tick);
    void heat_update();
    void accel_heat_update(); // GPU and NPU units
    void bit_rot();
    void cpu_wear();
    void accel_wear();
    void volatility(); // Phase 4

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
    // This tick's heat inputs. Writes after the tick's heat update (async
    // accelerator results at the barrier, actions landing) count toward the
    // next tick's, so they are state: saved, and hashed when any are pending.
    Array<u32> page_fetch_, page_write_;

  public:
    bool pages_pending() const; // any heat inputs carried into the next tick

  private:

    // GPU and NPU units (world state; empty without one)
    UnitLedger gpu_, npu_;

    // asynchronous accelerator ops (Phase 5): this tick's issued ops and their
    // addresses (taken at the barrier), and per CPU slot its generation, last
    // result and ops still pending (world state when async)
    Array<AccelOp> issued_;
    Array<u32> issue_ref_;  // per issued op: where its addresses start
    Array<u32> issue_addr_; // src, dst-read, matrix, then write addresses, per op
    u64 accel_serial_ = 0;
    Array<u32> cpu_gen_, cpu_result_, cpu_pending_;

    // signals (per-tick working state; empty without channels)
    Array<i64> emit_;
    Array<i32> field_;
    Array<u64> emit_count_, emit_amp_;

    // salvage stamps (world state; empty without the rule)
    Array<u32> stamp_;
    // Codons of block k still owned by it that have stamp 0 take stamp g.
    void stamp_block(u32 k, u32 g);

    // disk (world state; empty without one)
    DiskLedger disk_;
    const char* disk_folder_ = nullptr; // Phase 7b: its files' folder, or RAM
    Array<u32> disk_addr_; // scratch: a DISK op's addresses
    Array<u8> wiped_pg_;   // scratch: pages wiped this tick
    Array<u32> wiped_bodies_;

    // dispersal: this tick's departures (taken by the world at the barrier)
    DispersalPorts ports_;
    Array<Departure> departures_;
    Array<u8> departure_codons_;

    // warps (Phase 8; world state when gpu.warps is on, empty otherwise)
    Array<Warp> warps_;
    Array<u32> free_warps_;
    Array<Lane> lanes_;        // warp w's lanes at w x warp.lanes
    Array<u32> warp_head_, warp_count_; // per body slot
    Array<float> warp_carry_;  // per body slot: fractional warp work (or debt) carried over
    // warp working state (per tick, not world state)
    Array<double> warp_upkeep_; // per body: upkeep instructions its warps pay this tick
    usize sched_start_ = 0, sched_n_ = 0; // this tick's schedule over order_
    Array<u32> wimg_, wcst_, wrec_, wbody_, wwarp_; // the round's image, records, their bodies and warps
    Array<u32> wleft_, wsteps_; // per warp slot: quota left, steps run this tick
    Array<u8> wagain_;          // per warp slot: its host instruction was done, run it again this tick
    Array<u64> wkeys_;          // scratch: a body's blocks by start
    Array<u32> wlens_;
    Array<u32> wlive_, wact_;   // per warp slot: lanes not halted / nor yielded (follow the lanes)
    Array<u8> wstate_;          // per warp slot: kLanesHost, kLanesDevice or kLanesBoth
    u32 wchain_ = 0;            // the last round's longest body, in steps
    Array<u8> wrec_gpu_;        // per record of the round: the GPU runs it (the per-body split)
    Array<u32> wbody_chain_;    // per body slot: its warp steps in its last first round
    u32 wround_ = 0;            // the round warp_pack packed
    // Phase 9b, timed tables: per warp slot, its own clock's time used this
    // tick (working state), and the tick it may run again after a host
    // instruction or an action (world state, as the warps are).
    Array<u32> wtime_;
    Array<u64> wuntil_;
    void resolve_warp_actions(); // round 0: warps' actions whose end has come
    void count_lanes(u32 w);    // wlive_, wact_ from the host's lanes
    u32 new_warp(u32 body, u32 ip, const Cpu* launcher);
    void free_warp(u32 w);
    void release_warp_slot(u32 w);
    void kill_warps(u32 b);
    void merge_warps(u32 prey, u32 eater);
    u32 warp_adv(u32 body, u32 a, u32 n) const; // warp_isa.inc's w_adv, on the soup
    u32 birth(u32 body, u32 start, u32 form);   // a new body's first CPU or warp
    bool cpu_runs(u32 b) const;
    bool warp_runs(u32 b) const;
    bool sense_reading(u32 b, u32 r, u32 ci, u32* v) const;
    u32 action_reading(u32 r, u32 i) const; // SENSE 22/23 from action i (kNone: none)
    Array<u32> wact_soon_; // warp_pack's scratch: per body, the soonest live action on it (action_on, at once)
    // A warp's host instruction; or, with `resolving`, the end of its action.
    // Returns whether it succeeded.
    bool host_step(u32 w, const Action* resolving = nullptr);
    void build_warp_consts();
    // Phase 9b: per body slot, the tick + 1 its batch of GPU (NPU) ops opened
    // (working state): the first op of a tick pays the device's start-up.
    Array<u64> gpu_batch_, npu_batch_;
    bool batch_open(Array<u64>& batch, u32 body); // true if already open this tick; opens it

    // Phase 9c: per CPU slot, the kind of core its thread prefers (world state
    // with cpu.threads or E millicores; empty otherwise)
    Array<u8> cpu_kind_;
    bool kinds_on() const { return p_->kinds; }
    // scratch: run_body's per-thread time (total, and its E part) and time used
    Array<i64> tq_, tu_, tqe_, tue_;
    Array<u32> grp_; // scratch: one kind's threads
    // Phase 9c-b2, E millicores (design.md 6.15): units from e_first_ on are E
    // (the last round(millicores x cpu.e_share)). Their free pool (a free
    // unit's pool_pos_ is its place in its own kind's pool), and each body's
    // count of them: world state when the node has any. With none, pool_
    // holds every unit as before and these stay empty.
    u32 e_first_ = 0xFFFFFFFFu;
    Array<u32> epool_;
    Array<u32> eunits_;
    bool ek() const { return e_first_ < unit_owner_.size(); } // the node has E millicores
    bool is_e(u32 u) const { return u >= e_first_; }
    // per body slot, this tick (working state): its E units' speed and heat
    // input, and its work on them
    Array<float> espeed_, eheat_;
    Array<u32> ework_;
    Array<u32> cpu_time_, cpu_etime_; // per CPU slot, its last tick's time (working state, the viewer's)
    // run_body's share of a body's time among its threads (R3): each first up
    // to a core of its preferred kind, then the other kind's leftovers. A core
    // gives cap of P time or cap_e of E (equal unless the substrate's P and E
    // speeds differ: docs/world-plan.md W1).
    void share_time(usize k, i64 tp, i64 te, i64 cap, i64 cap_e, u64 tick);

    // Phase 10 (design.md 6.16). decay(): dead matter loses bits. vents():
    // each vent writes its codons into nearby waste. end_thread(): a halted
    // thread leaves its body. last_ran_: per body slot, the last tick it ran
    // an instruction, waited on an action or was born (world state with
    // life.frozen_ticks). vent_at_: the vents' places (from the seed).
    void decay();
    void vents();
    void end_thread(u32 b, u32 c);
    Array<u64> last_ran_;
    Array<u32> vent_at_;

    // Dormancy and drift (docs/dormancy-plan.md). dorm_: per body slot (world
    // state with life.dormancy); dorm_tag_: the lineage tag each waits with
    // (detector data, like a cargo's: not hashed or saved). go_dormant(): a
    // frozen body keeps its code and reserves, leaves the rest, and drifts on
    // (or rests here with nowhere to go); false when it has no reserves (it
    // dies). rest_tick(): a resting one's end of tick (wake, clock, dwell).
    // depart_dormant(): it leaves for neighbour nbr. drift_port(): a random
    // open neighbour from this node's stream, or kNone.
    Array<Dormant> dorm_;
    Array<CargoTag> dorm_tag_;
    u64 frozen_bare_ = 0, dormant_mem_sum_ = 0, dormant_disk_sum_ = 0; // diagnostics
    bool go_dormant(u32 b);
    void rest_tick(u32 b);
    void depart_dormant(u32 b, u32 nbr);
    u32 drift_port();
    bool can_wake(u32 b) const;
    void wake(u32 b);

  public:
    // Phase 10: the node's vents (their places in memory; none without them).
    const Array<u32>& vent_places() const { return vent_at_; }

    // Profiling (`nova run --profile`): when on, each part of this node's
    // tick adds its wall time here, in seconds. Working state that changes
    // nothing in the world.
    struct Timing {
        double first;  // vents, sparks, heat and GPU prep: tick_first without the bodies
        double bodies; // the bodies' CPUs (run_body)
        double pack, ref, unpack, host; // warps: packing, the CPU reference's run, unpacking, host instructions
        double second; // heat, hazards, decay and the death sweep (tick_second)
    };
    void set_timing(bool on) { timing_on_ = on; }
    Timing& timing() { return timing_; }

  private:
    bool timing_on_ = false;
    Timing timing_ = {};

  public:
    // Phase 9c: a body's E millicores (0 without them), and the node's.
    u32 e_units(u32 b) const { return b < eunits_.size() ? eunits_[b] : 0; }
    u32 e_total() const { return ek() ? static_cast<u32>(unit_owner_.size() - e_first_) : 0; }
    // Phase 9c, the viewer: CPU c's time last tick, and its part on E millicores
    // (picoseconds with a timed table; 0 without threads or E millicores).
    u32 cpu_time(u32 c) const { return c < cpu_time_.size() ? cpu_time_[c] : 0; }
    u32 cpu_etime(u32 c) const { return c < cpu_etime_.size() ? cpu_etime_[c] : 0; }
    u32 free_e_units() const { return static_cast<u32>(epool_.size()); }
    u32 e_held() const { // E millicores held by bodies
        return e_first_ < unit_owner_.size() ? static_cast<u32>(unit_owner_.size() - e_first_ - epool_.size()) : 0;
    }

  private:

    // actions in progress (Phase 9; world state with a timed cost table)
    Array<Action> actions_;
    void sweep_actions(); // drops those whose actor died or was eaten (after the death sweep)

    // per-tick working state (not part of the world state)
    Lineages* obs_ = nullptr;
    u64 tick_ = 0;
    SoupTally tally_ = {};
    double flaw_left_ = 0, copy_left_ = 0; // hazard countdowns, redrawn every tick
    float ambient_ = 0;                    // this tick's ambient heat input
    double hazard_mult_ = 1.0;             // the lifecycle's decline, this tick (1 when off)
    // the lifecycle (world state when it's on)
    u64 writes_ = 0;       // codon writes this life
    bool collapse_ = false;
    u64 arrived_dry_ = 0, arrived_all_ = 0; // arrived_dry(), arrived_all()
    void lifecycle_check();
    Array<u32> scratch_;
    u64 free_epoch_ = 0;         // bumped whenever a codon becomes waste
    u64 fail_epoch_ = ~0ull;     // see known_full()
    u32 fail_len_ = 0;
};

} // namespace nova
