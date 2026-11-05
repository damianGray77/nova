// The Phase 0 gate (design.md 9.1): the same seed gives an identical
// world-state hash at 1, 4 and 20 threads. Phase 1a runs it twice: on the
// Phase 0 demo world (whose hash must not move) and on a busy soup world with
// the interpreter, heat, hazards and contact all running.
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

void soup_gate() {
    const u32 nodes = 96; // must match kSoupWorld
    u32 reversed[nodes], shuffled[nodes];
    make_orders(nodes, reversed, shuffled);

    Trace t1 = trace(kSoupWorld, 1, nullptr);
    Trace t4 = trace(kSoupWorld, 4, nullptr);
    Trace t20 = trace(kSoupWorld, 20, nullptr);
    Trace t20_rev = trace(kSoupWorld, 20, reversed);
    Trace t20_shuf = trace(kSoupWorld, 20, shuffled);
    Trace t1_shuf = trace(kSoupWorld, 1, shuffled);

    CHECK(same(t1, t4, "soup: 1 vs 4 threads"));
    CHECK(same(t1, t20, "soup: 1 vs 20 threads"));
    CHECK(same(t1, t20_rev, "soup: 1 thread vs 20 threads reversed dispatch"));
    CHECK(same(t1, t20_shuf, "soup: 1 thread vs 20 threads shuffled dispatch"));
    CHECK(same(t1, t1_shuf, "soup: 1 thread vs 1 thread shuffled dispatch"));

    // Not vacuous: every mechanism ran.
    const SoupTally& s = t1.soup;
    CHECK(s.instructions > 0 && s.births > 0 && s.deaths > 0 && s.sparks > 0 && s.halts > 0);
    CHECK(s.copy_errors > 0 && s.flaws > 0 && s.rot_flips > 0);
    CHECK(s.cpu_released > 0 && s.mem_released > 0);
    CHECK(s.engulf_tries > 0 && s.inject_tries > 0 && s.engulfs > 0 && s.injects > 0);
    CHECK(t1.hash[0] != t1.hash[kPoints - 1]);

    printf("  gate: soup world, %u nodes, %llu ticks: hash %016llx at 1/4/20 threads "
           "(%llu instructions, %llu births, %llu deaths, %llu engulfs, %llu injects)\n",
           nodes, static_cast<unsigned long long>(kTicks),
           static_cast<unsigned long long>(t1.hash[kPoints - 1]),
           static_cast<unsigned long long>(s.instructions),
           static_cast<unsigned long long>(s.births), static_cast<unsigned long long>(s.deaths),
           static_cast<unsigned long long>(s.engulfs), static_cast<unsigned long long>(s.injects));
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

    // The demo world's hash is pinned: Phase 1a must not have moved it.
    CHECK_EQ_U64(t1.hash[kPoints - 1], 0xbcaf7639570ecb07ull);

    soup_gate();
}
