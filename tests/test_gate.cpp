// The Phase 0 gate (design.md 9.1): the same seed gives an identical
// world-state hash at 1, 4 and 20 threads. It runs three times: on the Phase 0
// demo world (whose hash must not move), on a busy soup world under the Phase
// 1a rules with the interpreter, heat, hazards and contact all running, and on
// a busy Phase 1b world that adds memory upkeep, CPU wear, sparks last, the
// doubled-invasion table and the replicator detector, whose gate events must
// match too, and a world with the in-world GPU (Phase 2).
//
// Beyond the three required thread counts, the gate also runs 20 threads with
// the shared queue's dispatch order reversed and shuffled. A clean timing run
// can pass by luck (threads happen to finish in index order); a permuted queue
// cannot -- so this proves the result is independent of *which* node runs
// first, not merely of how fast threads are.
//
// Checkpoints every 25 ticks are compared, not just the final hash, so a
// failure names the first tick where the runs diverged.
#include "check.h"
#include "helpers.h"
#include "nova/hash.h"
#include "nova/rng.h"

using namespace nova;

namespace {

constexpr u64 kTicks = 300;
constexpr u64 kEvery = 25;
constexpr usize kPoints = kTicks / kEvery + 1;

struct Trace {
    u64 hash[kPoints] = {};
    u64 refused = 0;
    u64 bounced = 0;
    SoupTally soup = {}; // summed over the run (soup model)
    usize events = 0;    // detector events, and a hash of their ticks and text
    u64 event_hash = 0;
    u64 colonists = 0;   // colonist events (Phase 3)
    bool ok = false;
};

Trace trace(const char* cfg, u32 threads, const u32* order) {
    Trace t;
    Run run;
    if (!make_run(run, cfg, threads))
        return t;
    run.set_dispatch_order(order);
    usize k = 0;
    for (u64 tick = 0;; ++tick) {
        if (tick % kEvery == 0)
            t.hash[k++] = run.world().hash();
        if (tick == kTicks)
            break;
        run.step();
        t.refused += run.world().last_tick().parcels_refused;
        t.bounced += run.world().last_tick().units_bounced;
        u64* sum = reinterpret_cast<u64*>(&t.soup);
        const u64* add = reinterpret_cast<const u64*>(&run.world().last_soup_tick());
        for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
            sum[i] += add[i];
    }
    Hasher h(0x6576656E74730000ull); // "events"
    for (usize i = 0; i < run.event_count(); ++i) {
        h.word(run.event_tick(i));
        h.bytes(run.event_text(i).p, run.event_text(i).n);
        t.colonists += run.event_text(i).starts_with("colonist ");
    }
    t.events = run.event_count();
    t.event_hash = h.finish();
    t.ok = true;
    return t;
}

// Returns true if identical; otherwise reports the first divergent tick.
bool same(const Trace& a, const Trace& b, const char* label) {
    if (!a.ok || !b.ok) {
        fprintf(stderr, "  gate: %s: run failed to start\n", label);
        return false;
    }
    for (usize k = 0; k < kPoints; ++k) {
        if (a.hash[k] != b.hash[k]) {
            fprintf(stderr, "  gate: %s diverges first at tick %llu (%016llx vs %016llx)\n", label,
                    static_cast<unsigned long long>(k * kEvery),
                    static_cast<unsigned long long>(a.hash[k]),
                    static_cast<unsigned long long>(b.hash[k]));
            return false;
        }
    }
    if (a.events != b.events || a.event_hash != b.event_hash) {
        fprintf(stderr, "  gate: %s: detector events differ (%llu vs %llu)\n", label,
                static_cast<unsigned long long>(a.events), static_cast<unsigned long long>(b.events));
        return false;
    }
    return true;
}

void make_orders(u32 nodes, u32* reversed, u32* shuffled) {
    for (u32 i = 0; i < nodes; ++i) {
        reversed[i] = nodes - 1 - i;
        shuffled[i] = i;
    }
    Rng r(7, 0, 0); // Fisher-Yates with a fixed seed
    for (u32 i = nodes - 1; i > 0; --i) {
        u32 j = static_cast<u32>(r.below(i + 1));
        u32 tmp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = tmp;
    }
}

enum class Rules { phase1a, phase1b, gpu, accel, world, phase4, async };

void soup_gate(const char* name, const char* cfg, u32 nodes, Rules rules) {
    u32 reversed[256], shuffled[256];
    make_orders(nodes, reversed, shuffled);

    Trace t1 = trace(cfg, 1, nullptr);
    Trace t4 = trace(cfg, 4, nullptr);
    Trace t20 = trace(cfg, 20, nullptr);
    Trace t20_rev = trace(cfg, 20, reversed);
    Trace t20_shuf = trace(cfg, 20, shuffled);
    Trace t1_shuf = trace(cfg, 1, shuffled);

    char label[96];
    snprintf(label, sizeof label, "%s: 1 vs 4 threads", name);
    CHECK(same(t1, t4, label));
    snprintf(label, sizeof label, "%s: 1 vs 20 threads", name);
    CHECK(same(t1, t20, label));
    snprintf(label, sizeof label, "%s: 1 thread vs 20 threads reversed dispatch", name);
    CHECK(same(t1, t20_rev, label));
    snprintf(label, sizeof label, "%s: 1 thread vs 20 threads shuffled dispatch", name);
    CHECK(same(t1, t20_shuf, label));
    snprintf(label, sizeof label, "%s: 1 thread vs 1 thread shuffled dispatch", name);
    CHECK(same(t1, t1_shuf, label));

    // Not vacuous: every mechanism ran.
    const SoupTally& s = t1.soup;
    CHECK(s.instructions > 0 && s.births > 0 && s.deaths > 0 && s.sparks > 0 && s.halts > 0);
    CHECK(s.copy_errors > 0 && s.flaws > 0 && s.rot_flips > 0);
    CHECK(t1.hash[0] != t1.hash[kPoints - 1]);
    const bool accel = rules == Rules::gpu || rules == Rules::accel || rules == Rules::world ||
                       rules == Rules::phase4 || rules == Rules::async;
    if (!accel) { // the accelerator and world worlds run default heat: no releases forced
        CHECK(s.cpu_released > 0 && s.mem_released > 0);
        CHECK(s.engulf_tries > 0 && s.inject_tries > 0 && s.engulfs > 0 && s.injects > 0);
    }
    if (rules == Rules::phase1a)
        CHECK(s.upkeep == 0 && s.cpu_worn == 0 && s.gpu_elements == 0 && s.npu_macs == 0);
    else
        CHECK(s.upkeep > 0 && s.cpu_worn > 0 && t1.events > 0);
    if (rules == Rules::gpu)
        CHECK(s.gpu_elements > 0 && s.gpu_worn > 0);
    if (rules == Rules::accel || rules == Rules::async)
        CHECK(s.npu_macs > 0 && s.npu_worn > 0);
    if (rules == Rules::phase4) { // every Phase 4 feature fired
        CHECK(s.sent > 0 && s.arrived > 0 && s.corrupted > 0 && s.transit_lost > 0);
        CHECK(s.wipes > 0 && s.disk_rot > 0 && s.garbled > 0 && s.emits > 0 && s.listens > 0);
        CHECK(s.collapses > 0 && s.recoveries > 0);
        printf("  gate: %s: %llu corrupted, %llu lost in transit, %llu disk ops, %llu wipes, %llu garbled, "
               "%llu emits, %llu collapses, %llu recoveries\n",
               name, static_cast<unsigned long long>(s.corrupted), static_cast<unsigned long long>(s.transit_lost),
               static_cast<unsigned long long>(s.disk_stores + s.disk_loads + s.disk_compares),
               static_cast<unsigned long long>(s.wipes), static_cast<unsigned long long>(s.garbled),
               static_cast<unsigned long long>(s.emits), static_cast<unsigned long long>(s.collapses),
               static_cast<unsigned long long>(s.recoveries));
    }
    if (rules == Rules::world) { // dispersal, transit, arrivals, and colonists
        CHECK(s.sent > 0 && s.arrived > 0 && t1.colonists > 0);
        printf("  gate: %s: %llu sent, %llu arrived, %llu lost, %llu colonist events\n", name,
               static_cast<unsigned long long>(s.sent), static_cast<unsigned long long>(s.arrived),
               static_cast<unsigned long long>(s.lost), static_cast<unsigned long long>(t1.colonists));
    }

    printf("  gate: %s, %u nodes, %llu ticks: hash %016llx at 1/4/20 threads "
           "(%llu instructions, %llu births, %llu deaths, %llu engulfs, %llu injects, %llu gate "
           "events)\n",
           name, nodes, static_cast<unsigned long long>(kTicks),
           static_cast<unsigned long long>(t1.hash[kPoints - 1]),
           static_cast<unsigned long long>(s.instructions),
           static_cast<unsigned long long>(s.births), static_cast<unsigned long long>(s.deaths),
           static_cast<unsigned long long>(s.engulfs), static_cast<unsigned long long>(s.injects),
           static_cast<unsigned long long>(t1.events));
}

} // namespace

void suite_gate() {
    const u32 nodes = 200; // must match kBusyWorld
    u32 reversed[nodes], shuffled[nodes];
    make_orders(nodes, reversed, shuffled);

    Trace t1 = trace(kBusyWorld, 1, nullptr);
    Trace t4 = trace(kBusyWorld, 4, nullptr);
    Trace t20 = trace(kBusyWorld, 20, nullptr);
    Trace t20_again = trace(kBusyWorld, 20, nullptr);
    Trace t20_rev = trace(kBusyWorld, 20, reversed);
    Trace t20_shuf = trace(kBusyWorld, 20, shuffled);
    Trace t1_shuf = trace(kBusyWorld, 1, shuffled);

    // The gate.
    CHECK(same(t1, t4, "1 vs 4 threads"));
    CHECK(same(t1, t20, "1 vs 20 threads"));
    // Order independence and repeatability.
    CHECK(same(t20, t20_again, "20 threads, repeated"));
    CHECK(same(t1, t20_rev, "1 thread vs 20 threads reversed dispatch"));
    CHECK(same(t1, t20_shuf, "1 thread vs 20 threads shuffled dispatch"));
    CHECK(same(t1, t1_shuf, "1 thread vs 1 thread shuffled dispatch"));

    // The order-sensitive paths actually ran, so the gate is not vacuous.
    CHECK(t1.refused > 0);
    CHECK(t1.bounced > 0);
    // The hash really tracks the world: ticks differ, and another seed differs.
    CHECK(t1.hash[0] != t1.hash[kPoints - 1]);
    Trace other = trace("seed = 43\nworld.nodes = 200\nworld.chords = 100\n"
                        "world.soup_size = 1024\n",
                        20, nullptr);
    CHECK(other.ok && other.hash[kPoints - 1] != t1.hash[kPoints - 1]);

    printf("  gate: seed 42, %u nodes, %llu ticks: hash %016llx at 1/4/20 threads "
           "(%llu refusals, %llu units bounced)\n",
           nodes, static_cast<unsigned long long>(kTicks),
           static_cast<unsigned long long>(t1.hash[kPoints - 1]),
           static_cast<unsigned long long>(t1.refused),
           static_cast<unsigned long long>(t1.bounced));

    // The demo world's hash is pinned: Phases 1a and 1b must not have moved it.
    CHECK_EQ_U64(t1.hash[kPoints - 1], 0xbcaf7639570ecb07ull);

    TextBuf phase1a;
    phase1a.append(kSoupWorld);
    phase1a.append(kPhase1aRules);
    soup_gate("soup world (1a rules)", phase1a.c_str(), 96, Rules::phase1a);
    soup_gate("soup world (1b rules)", kSoupWorld1b, 24, Rules::phase1b);
    soup_gate("soup world (GPU)", kSoupWorldGpu, 24, Rules::gpu);
    soup_gate("soup world (GPU + NPU)", kSoupWorldAccel, 24, Rules::accel);
    soup_gate("procedural world (dispersal)", kSoupWorldProc, 24, Rules::world);
    TextBuf p4;
    p4.append(kSoupWorldProc);
    p4.append(kPhase4All);
    soup_gate("Phase 4 world (every feature)", p4.c_str(), 24, Rules::phase4);
    // Phase 5: async accelerator ops, gated on the CPU reference; then the
    // same world on the real devices (where present) at 20 threads must match
    // it tick for tick.
    TextBuf acc;
    acc.append(kSoupWorldAccel);
    acc.append("accel.mode = async\naccel.gpu = cpu\naccel.npu = cpu\n");
    soup_gate("async accelerator world (CPU reference)", acc.c_str(), 24, Rules::async);
    TextBuf dev;
    dev.append(kSoupWorldAccel);
    dev.append("accel.mode = async\n");
    Trace on_cpu = trace(acc.c_str(), 1, nullptr);
    Trace on_dev = trace(dev.c_str(), 20, nullptr);
    CHECK(same(on_cpu, on_dev, "async world: CPU reference at 1 thread vs the devices at 20 threads"));
}
