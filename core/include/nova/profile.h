#pragma once

// Behaviour profiles (docs/analysis-plan.md A3; docs/results/profiles.md):
// what programs do, not what they look like. A pure observer, like the
// lineage detector (lineage.h).
//
// Sampling. Every profile.every ticks (at the start of each multiple of it),
// each node picks profile.per_node of its living bodies (resting dormant ones
// aside) and follows each for profile.ticks ticks, or until it dies or goes
// dormant. The picks come from the observer's own stream, Rng(seed ^
// "profiles", node, tick) -- never a world stream -- so the same run gives the
// same samples at any thread count.
//
// What a sample records (ProfileSample): instructions run, by op; a bitmap of
// the codons of its genome (its first block) that ran; its actions -- ENGULF
// and INJECT tried and landed, bites of raw material, DIVIDE, dispersals,
// going dormant, DISK by kind and tier and loads into its own block, EMIT and
// LISTEN with what LISTEN heard, GPU and NPU ops, warp launches and threads
// started --; what it holds (last seen); its lineage, genotype and founder
// origin; its memory neighbours at the start (kin clustering); and, for each
// LISTEN, which way the next conditional branch went (listening then acting).
//
// How it watches. The interpreter asks watching() once per thread per tick
// (run_cpu) and runs a sampled thread through a step that also records into
// the sample (ProfileProbe: the soup's tallies before and after the
// instruction tell what it did). Warps: a warp's host instructions (its
// contact, DISK, DIVIDE, signals, GPU verbs ...) are recorded by op like a
// CPU's, its lane instructions only as a total (warp_unpack), and its lanes'
// codons only where they stand at each tick's end; the GPU kernel itself is
// not touched. So a GPU-born body's bitmap, and its ops, are a lower bound.
//
// Observe-only. Nothing here writes to a soup or draws from a world stream,
// and the sampling state is neither hashed nor saved in snapshots: a resumed
// run starts sampling afresh at the next multiple of profile.every, as the
// detector starts afresh. Finished samples wait in each node's buffer until
// the world collects them at the barrier, in node order (so profiles.csv is
// the same at any thread count).
#include "nova/array.h"
#include "nova/config.h"
#include "nova/isa.h"
#include "nova/soup.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// The profile.* settings (keys.inc), read before World::init's check returns.
struct ProfileParams {
    bool enabled = false;
    u32 every = 1000;   // profile.every: ticks between sampling rounds
    u32 per_node = 4;   // profile.per_node: bodies followed per node per round
    u32 ticks = 50;     // profile.ticks: ticks each is followed (at most profile.every)
    bool arrivals = true; // profile.arrivals: log arrivals during each round (arrivals.csv)
};
// Reads profile.* (marking them used); a bad value is reported through cfg.
bool profile_params_from_config(Config& cfg, ProfileParams* p);

// A sample's actions, counted over the ticks it was followed.
enum : u32 {
    kPaEngulfTry,     // ENGULF begun (aimed or blind)
    kPaEngulf,        // ... that took a body: whole, or a bite of one (predation)
    kPaBiteWaste,     // ... that bit raw material instead (scavenging)
    kPaInjectTry,     // INJECT begun
    kPaInject,        // ... that landed
    kPaDivide,        // DIVIDE here
    kPaDisperse,      // DIVIDE to a neighbour
    kPaDormant,       // went dormant (the sample ends there)
    kPaDiskCompare,   // DISK ops done, by kind ...
    kPaDiskStore,
    kPaDiskLoad,
    kPaArchiveStore,  // ... of them touching the archive (disk tiers, W4)
    kPaArchiveLoad,
    kPaSelfLoad,      // ... loads that wrote into the block it runs in (repair)
    kPaEmit,          // EMIT done
    kPaListen,        // LISTEN done
    kPaHeard,         // ... that heard something (not 0)
    kPaGpu,           // accelerator ops issued on the GPU (BX 0-15)
    kPaNpu,           // ... on the NPU (BX 16-19)
    kPaLaunch,        // warps launched (BX 20)
    kPaThread,        // threads started (BX 21)
    kPaCount
};
// The column names, profiles.csv (engulf_tries, engulf_landed, ...).
const char* profile_action_name(u32 a);

// What it holds, last seen alive (each tick's end, or as it went dormant).
enum : u32 { kHoldMem, kHoldCpu, kHoldEcpu, kHoldGpu, kHoldNpu, kHoldDisk, kHoldArchive, kHoldThreads, kHoldWarps,
             kHoldCount };
const char* profile_hold_name(u32 h); // hold_mem, hold_cpu, ...

// How a sample ended.
inline constexpr u32 kProfileDone = 0;    // followed for profile.ticks
inline constexpr u32 kProfileGone = 1;    // died, or was eaten
inline constexpr u32 kProfileDormant = 2; // went dormant
const char* profile_end_name(u32 why);   // done, gone, dormant

// Memory neighbours (kin clustering): bits of nb_left / nb_right.
inline constexpr u32 kNbFound = 1;    // another living body within kProfileNeighbourScan codons that side
inline constexpr u32 kNbLineage = 2;  // ... of the same lineage (both members)
inline constexpr u32 kNbGenotype = 4; // ... with the same genome
inline constexpr u32 kProfileNeighbourScan = 512;

inline constexpr u32 kProfileGenomeMax = 2048; // codons a sample's bitmap covers
inline constexpr u32 kLtaWindow = 16; // a LISTEN waits this many instructions for its branch

struct ProfileSample {
    u64 serial;       // the body's serial on its node
    u64 start, end;   // the first and the last tick it was followed
    u64 lineage;      // its lineage (lineage.h), 0 = not a member (or detection off)
    u64 genotype;     // genotype_hash of its genome as it began
    u64 instr;        // instructions it ran: its threads', and its warps' host instructions
    u64 lane_instr;   // its warps' lane instructions
    u64 heard_abs;    // the |values| LISTEN heard, summed
    u32 node, body;   // where: node, body slot
    u32 block, gstart, glen; // its genome block (slot), start and length
    u32 depth;        // its lineage depth
    u32 origin;       // how it began (kOrigin...)
    u32 founder_origin; // how its lineage's founder began
    u32 form;         // kFormCpu or kFormGpu
    u32 why;          // how it ended (kProfile...)
    u32 foreign;      // tries to run codons it doesn't own
    u32 faults;       // instructions that failed
    u32 act[kPaCount];
    // Listening then acting: per LISTEN, its heard value's class (0 zero,
    // 1 positive, 2 negative) x which way the next conditional branch within
    // kLtaWindow instructions went (0 fell through, 1 the other way).
    u32 lta[6];
    u32 hold[kHoldCount];
    // Kin clustering, at the start: the node's living bodies (resting
    // dormant ones aside), members of its lineage among them (itself
    // included; 0 if not a member), bodies with its genome, and its nearest
    // neighbour on each side in memory (kNb... bits).
    u32 node_bodies, lin_alive, geno_alive, nb_left, nb_right;
    u32 lt_wait, lt_class; // working: a LISTEN waiting for its branch (instructions since, 0 none)
    u32 ops[kOpCount];     // instructions run, by op
    u8 bits[kProfileGenomeMax / 8]; // genome codons its CPUs ran (codon i: bit i % 8 of byte i / 8; lanes not)
};

// --- the live census's fixed rules ------------------------------------------
// One role per sample, the first that applies (docs/results/profiles.md):
//   predator   took a body (ENGULF, whole or a bite)
//   parasite   landed an INJECT
//   repairer   loaded its own code from disk
//   signaller  emitted
//   wanderer   went dormant, or sent more daughters away than it kept
//   copier     divided (here or to a neighbour)
//   idle       none of these
enum : u32 { kRolePredator, kRoleParasite, kRoleRepairer, kRoleSignaller, kRoleWanderer, kRoleCopier, kRoleIdle,
             kRoleCount };
const char* role_name(u32 r);
u32 role_by_rules(const u32* act); // act: kPaCount counts

// --- honest complexity --------------------------------------------------------
u32 profile_working_length(const ProfileSample& s); // genome codons that ran
u32 profile_distinct_ops(const ProfileSample& s);   // ops run at least once
// Kinds of action used (out of 15: ENGULF, bite of waste, INJECT, DIVIDE,
// dispersal, dormancy, DISK compare, store and load, EMIT, LISTEN, GPU, NPU,
// launch, thread); tries count, the tiers and repairs are kinds of store and load.
u32 action_kinds(const u32* act);
inline constexpr u32 kActionKinds = 15;

// One sampling round's census (the live stats columns): its samples' roles
// by the rules above, and their working lengths and action kinds, summed.
struct ProfileCensus {
    u64 round = ~0ull; // the tick its samples began (~0: none yet)
    u32 samples = 0;
    u32 role[kRoleCount] = {};
    u64 work_len = 0, kinds = 0;
    void add(const ProfileSample& s);
};

// --- recording an instruction (the interpreter's side) ----------------------
inline constexpr u32 kProbeTallies = 17;
struct ProfileProbe {
    u64 t[kProbeTallies]; // the soup's tallies before the instruction (see profile.cpp)
    u32 bx;               // BX before it (the selectors)
    u32 dflt;             // the address after it: where the IP goes if nothing branches
    u32 block;            // the block its codon belonged to when fetched
    void before(const SoupTally& tl, u32 bx_, u32 dflt_, u32 block_);
};
// After one instruction (or an action's end: resolving, not counted again as
// an instruction): what it did, from the tallies' change, op and BX.
void profile_step(ProfileSample& s, const ProfileProbe& pr, const SoupTally& tl, Op op, u32 ip, bool ok, u32 next,
                  u32 cx, bool resolving, bool self_load);
// A try to run a codon the body doesn't own (private code): an instruction
// that faults.
void profile_foreign(ProfileSample& s);

// --- one node's sampler ---------------------------------------------------------
class Profiles {
  public:
    void init(const ProfileParams* p, u64 seed, u32 node);
    // Forgets samples in progress (a snapshot loaded, the node's next life);
    // finished ones not yet collected are kept.
    void reset();

    // --- hooks ------------------------------------------------------------------
    // The tick's start (Soup::tick_first, before the bodies run): at a
    // multiple of profile.every, the round's picks.
    void on_tick_start(const Soup& s, u64 tick);
    // The tick's end (the world, at the barrier, after everything else):
    // holdings; samples that died, or have run their profile.ticks, end.
    void on_tick_end(const Soup& s, u64 tick);
    // Body b goes dormant (Soup::go_dormant, before it lets go of anything).
    void on_dormant(const Soup& s, u32 b);
    // Body b's warps ran lane instructions (Soup::warp_unpack).
    void on_lanes(const Soup& s, u32 b, u64 lane_instr) {
        if (ProfileSample* x = watching(s, b))
            x->lane_instr += lane_instr;
    }
    // The sample following body b, or nullptr (the interpreter asks once per
    // thread per tick).
    ProfileSample* watching(const Soup& s, u32 b) {
        ProfileSample* x = slot_of(b);
        return x && s.body(b).serial == x->serial ? x : nullptr;
    }

    // Tests: follow body b from now on, as a round's pick would (false if it
    // is already followed, dead or dormant, or the round is full).
    bool follow(const Soup& s, u32 b, u64 tick);
    // Tests: end every sample now (as followed to its end).
    void finish_all(u64 tick);

    // --- inspection and outputs ---------------------------------------------
    u32 active() const { return nactive_; }
    const Array<ProfileSample>& finished() const { return done_; }
    void clear_finished() { done_.clear(); }

  private:
    ProfileSample* slot_of(u32 b) {
        if (nactive_ == 0 || b >= slot_.size() || slot_[b] == kNone)
            return nullptr;
        return &live_[slot_[b]];
    }
    void start(const Soup& s, u32 b, u64 tick, bool census);
    void finish(u32 i, u32 why, u64 end);
    void holdings(const Soup& s, ProfileSample& x) const;

    const ProfileParams* p_ = nullptr;
    u64 seed_ = 0;
    u32 node_ = 0;
    Array<ProfileSample> live_; // samples in progress (used_ marks the live slots)
    Array<u8> used_;
    Array<u32> slot_;           // per body slot: its sample in live_, or kNone
    u32 nactive_ = 0;
    Array<ProfileSample> done_; // finished, waiting for the world
    // the round's node census (kin clustering) and scratch
    Array<u32> cand_;
    Array<u64> cgeno_, clin_;
};

// --- the arrivals log (moving together) -----------------------------------------
// A daughter (or a dormant program) placed on a node at the barrier, during a
// sampling round's ticks (profile.arrivals): arrivals.csv.
struct ArrivalRecord {
    u64 tick;     // the tick at whose end it arrived
    u64 lineage;  // its lineage tag (0: not a member, or detection off)
    u64 genotype; // genotype_hash of the codons that arrived
    u32 node, src, len, dormant;
};

// --- profiles.csv ------------------------------------------------------------------
// The header (one op_<NAME> column per distinct op of the codon table, in
// codon order, then the codon bitmap as hex, 4 codons a digit) and a row.
void profile_csv_header(const CodonTable& t, TextBuf& out);
void profile_csv_row(const CodonTable& t, const ProfileSample& s, TextBuf& out);
void arrivals_csv_header(TextBuf& out);
void arrivals_csv_row(const ArrivalRecord& a, TextBuf& out);

} // namespace nova
