// docs/lifehistory-plan.md (revision 2, approved by Damian 2026-11-08):
// gestation (G), ray hazards (R), wipe warnings (W) and voluntary dormancy (V).
#include <cmath>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// A quiet soup on the classic table (every instruction costs 1), so ticks
// count instructions simply.
struct LFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 1024) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("cost.table = classic\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 31, 0);
        s.begin_tick(0);
        return true;
    }
    // Runs ticks [from, to) and returns births so far (the tally is a tick's).
    u64 births = 0;
    u64 run(u64 from, u64 to) {
        for (u64 t = from; t < to; ++t) {
            s.tick(t);
            births += s.tally().births;
        }
        return births;
    }
    // The tick of the first birth from `from` on, or kNone64 within `limit` ticks.
    u64 first_birth(u64 from, u64 limit) {
        for (u64 t = from; t < from + limit; ++t) {
            const u64 before = births;
            run(t, t + 1);
            if (births > before)
                return t;
        }
        return ~0ull;
    }
};

// SENSE (BX 0: my size) sets CX to 3, ALLOC claims 3 codons as her block,
// DIVIDE hands it over; the body runs off its end back to SENSE.
const char* const kBudder = "SENSE ALLOC DIVIDE";

// G1: the first daughter comes exactly life.gestation ticks after the ALLOC
// that made her block, and each ALLOC restarts the clock (one birth every 5).
void test_gestation_cpu() {
    LFix off;
    CHECK(off.init(""));
    CHECK(plant(off.s, 100, kBudder, 1000) != kNone);
    CHECK(off.run(0, 1) > 1); // without gestation: births the first tick

    LFix f;
    CHECK(f.init("life.gestation = 5\n"));
    CHECK(plant(f.s, 100, kBudder, 1000) != kNone);
    CHECK_EQ_U64(f.run(0, 5), 0);  // ticks 0-4: she isn't ripe
    CHECK_EQ_U64(f.run(5, 6), 1);  // tick 5: the first birth, then a new ALLOC
    CHECK_EQ_U64(f.run(6, 10), 1); // ... which ripens at tick 10
    CHECK_EQ_U64(f.run(10, 11), 2);
    printf("  gestation: a budder divides at ticks 5 and 10 with life.gestation = 5 (at once without)\n");
}

// G2: the per-codon term adds to the wait (3 codons x 1 + 2 = 5 ticks).
void test_gestation_per_codon() {
    LFix f;
    CHECK(f.init("life.gestation = 2\nlife.gestation_per_codon = 1\n"));
    CHECK(plant(f.s, 100, kBudder, 1000) != kNone);
    CHECK_EQ_U64(f.run(0, 5), 0);
    CHECK_EQ_U64(f.run(5, 6), 1);
}

// A body waiting at its DIVIDE still runs an instruction each tick (the
// DIVIDE, again), so the frozen watchdog leaves it alone.
void test_gestation_not_frozen() {
    LFix f;
    CHECK(f.init("life.gestation = 20\nlife.frozen_ticks = 3\n"));
    const u32 b = plant(f.s, 100, kBudder, 1000);
    CHECK(b != kNone);
    CHECK_EQ_U64(f.run(0, 20), 0);
    CHECK(f.s.body(b).alive && f.s.tally().died_frozen == 0);
    CHECK_EQ_U64(f.run(20, 21), 1);
}

// G4: a warp's DIVIDE (a host instruction) waits the same way: the warp
// stays at it, lanes unchanged, and runs again once she is ripe.
void test_gestation_warp() {
    for (u32 g = 0; g < 2; ++g) {
        LFix f;
        TextBuf extra;
        extra.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes = 4\n"
                     "warp.step_cost = 1\nwarp.lane_cost = 0\nwarp.capacity = 50\n");
        if (g)
            extra.append("life.gestation = 5\n");
        CHECK(f.init(extra.c_str()));
        Array<u8> code;
        TextBuf err;
        CHECK(genome_from_names(f.p.table, kBudder, &code, &err));
        for (usize i = 0; i < code.size(); ++i)
            f.s.mem_mut()[100 + i] = code[i];
        CHECK(f.s.create_body_form(100, static_cast<u32>(code.size()), 0, 1, kFormGpu) != kNone);
        // A warp's ALLOC and DIVIDE are host instructions, each a round
        // after the last: without gestation it divides within a tick or two.
        const u64 first = f.first_birth(0, 20);
        if (!g)
            CHECK(first <= 2);
        else
            CHECK(first >= 5 && first <= 7);
        if (g)
            printf("  gestation: a warp budder's first daughter at tick %llu with life.gestation = 5\n",
                   static_cast<unsigned long long>(first));
    }
}

// --- Rays (R) ---------------------------------------------------------------

// A soup for rays: heat, copy errors and flaws off, rays on, and every codon
// a plain one (INC), so waste isn't NOPs (nova_v1's codon 0 is NOP0). The
// bodies planted here never run: no tick is needed to walk a ray.
struct RFix {
    SoupParams p;
    Soup s;
    u8 plain = 0, nop = 0;
    bool init(const char* extra, u32 size) {
        TextBuf cfg;
        cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nmem.upkeep = 0\ncost.table = classic\n");
        if (!strstr(extra, "hazard.rays ="))
            cfg.append("hazard.rays = true\n");
        if (!strstr(extra, "hazard.rot ="))
            cfg.append("hazard.rot = 0\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 31, 0);
        s.begin_tick(0);
        Array<u8> c;
        TextBuf err;
        if (!genome_from_names(p.table, "INC NOP0", &c, &err))
            return false;
        plain = c[0];
        nop = c[1];
        for (u32 a = 0; a < size; ++a)
            s.mem_mut()[a] = plain;
        return true;
    }
    // A body at `at`: `len` plain codons with `pad` NOPs on each side.
    u32 body(u32 at, u32 len, u32 pad) {
        for (u32 i = 0; i < len + 2 * pad; ++i)
            s.mem_mut()[at + i] = i < pad || i >= pad + len ? nop : plain;
        return s.create_body(at, len + 2 * pad, 1);
    }
    // Codons in [lo, lo + n) no longer plain.
    u32 changed(u32 lo, u32 n) const {
        u32 k = 0;
        for (u32 a = lo; a < lo + n; ++a)
            k += s.mem()[a] != plain ? 1 : 0;
        return k;
    }
};

// R1: a ray always starts outside allocated space. Drawn inside a block, it
// starts at the last unallocated codon behind it; with none, at that end of
// the soup. (Wipe rays, as lightning: every codon passed changes, so the run
// is exact: living 1, waste 2, NOPs 8 a codon.)
void test_ray_from_outside() {
    RFix f;
    CHECK(f.init("", 1024));
    CHECK(f.body(100, 40, 0) != kNone); // [100, 140)
    Rng r(7, 0, 0);
    Soup::RayRun h = f.s.ray(120, true, 10, false, true, r);
    CHECK(h.lo == 99 && h.n == 9); // waste 99 (2), then 100-107 (8)
    h = f.s.ray(120, false, 10, false, true, r);
    CHECK(h.lo == 132 && h.n == 9); // waste 140, then 139 down to 132
    h = f.s.ray(500, true, 10, false, true, r);
    CHECK(h.lo == 500 && h.n == 5); // drawn in waste: it starts there
    CHECK(f.body(0, 40, 0) != kNone);
    h = f.s.ray(20, true, 10, false, true, r);
    CHECK(h.lo == 0 && h.n == 10); // nothing unallocated behind: the soup's end
    CHECK(f.body(300, 20, 4) != kNone); // [300, 328), NOPs at 300-303 and 324-327
    h = f.s.ray(310, true, 34, false, true, r);
    CHECK(h.lo == 299 && h.n == 5); // waste (2) and 4 NOPs (32): none of the code
    // A strike drawn inside a block: a ray of its size, heading either way,
    // that starts outside the block and stops after its size in living code.
    CHECK(f.body(600, 400, 0) != kNone); // [600, 1000)
    const u32 got = f.s.strike(800, 64, r);
    CHECK_EQ_U64(got, 63); // the waste codon it starts on (2), then 62 living
    CHECK(f.changed(600, 62) + f.changed(938, 62) >= 58);  // (a random codon is plain 1 in 64)
    CHECK(f.changed(662, 276) == 0);
    printf("  rays: drawn inside a block, a ray starts outside it; 4 NOPs stop a ray of 34\n");
}

// R2, the calibration. (a) Through the tick: with dead matter absorbing as
// living code does and NOPs as plain codons (nothing shields), cosmic rays
// flip hazard.rot x codons x ticks. (b) In a soup of blocks and gaps, living
// codons take independent flips' damage.
void test_ray_calibration() {
    const char* even = "hazard.ray_cosmic = 64\nhazard.ray_flip = 0.0625\nhazard.ray_flip_nop = 0.0625\n"
                       "hazard.ray_absorb_waste = 1\nhazard.ray_absorb_nop = 1\n";
    {
        RFix f;
        TextBuf extra;
        extra.append(even);
        extra.append("hazard.rot = 0.001\n");
        CHECK(f.init(extra.c_str(), 65536));
        u64 flips = 0;
        const u64 T = 300;
        for (u64 t = 0; t < T; ++t) {
            f.s.tick(t);
            flips += f.s.tally().rot_flips;
        }
        const double want = 0.001 * 65536.0 * static_cast<double>(T);
        const double ratio = static_cast<double>(flips) / want;
        CHECK(ratio > 0.95 && ratio < 1.05);
        printf("  rays: cosmic flips %llu through the tick against %.0f expected (x%.3f)\n",
               static_cast<unsigned long long>(flips), want, ratio);
    }
    {
        RFix f;
        CHECK(f.init(even, 65536));
        for (u32 b = 0; b < 1024; ++b)
            CHECK(f.body(b * 64, 32, 0) != kNone);
        const double lambda = 0.3;
        Rng r(11, 0, 0);
        f.s.ray_shower(f.s.ray_rate(lambda, 64), 64, false, r);
        u32 hit = 0;
        for (u32 b = 0; b < 1024; ++b)
            hit += f.changed(b * 64, 32);
        // Flips per bit are Poisson(lambda / 6); a codon is unchanged when
        // every bit flipped an even number of times.
        const double even_bit = (1.0 + exp(-lambda / 3.0)) / 2.0;
        const double want = 32768.0 * (1.0 - pow(even_bit, 6));
        const double ratio = static_cast<double>(hit) / want;
        CHECK(ratio > 0.95 && ratio < 1.05);
        printf("  rays: living codons changed %u against %.0f with independent flips (x%.3f)\n", hit, want, ratio);
    }
}

// R3: shields. The same shower over the same plain 32-codon cores: padded
// with 4 NOPs each side they take fewer hits, and with dead matter around
// them absorbing more, fewer again.
u32 core_hits(const char* extra, u32 pad) {
    RFix f;
    if (!f.init(extra, 65536))
        return 0;
    for (u32 b = 0; b < 512; ++b)
        if (f.body(b * 128 + 48 - pad, 32, pad) == kNone)
            return 0;
    Rng r(13, 0, 0);
    f.s.ray_shower(f.s.ray_rate(0.2, 64), 64, false, r);
    u32 hit = 0;
    for (u32 b = 0; b < 512; ++b)
        hit += f.changed(b * 128 + 48, 32);
    return hit;
}

void test_ray_shields() {
    const char* base = "hazard.ray_flip = 0.0625\n";
    const u32 bare = core_hits(base, 0);
    const u32 padded = core_hits(base, 4);
    const u32 thick = core_hits("hazard.ray_flip = 0.0625\nhazard.ray_absorb_waste = 4\n", 0);
    CHECK(bare > 1000);
    CHECK(padded < bare * 0.9); // (about 0.83: rays drawn in the pads start outside them too)
    CHECK(thick < bare * 0.9);
    printf("  rays: core hits bare %u, with 4 NOPs each side %u (x%.2f), in denser dead matter %u (x%.2f)\n", bare,
           padded, static_cast<double>(padded) / bare, thick, static_cast<double>(thick) / bare);
}

// R2: storm static as rays, through the tick: low-energy rays at the weather's
// rate, calibrated as cosmic rays are.
void test_ray_static() {
    RFix f;
    CHECK(f.init("hazard.ray_absorb_waste = 1\nhazard.ray_absorb_nop = 1\nhazard.ray_flip_nop = 0.03125\n", 65536));
    Weather w{};
    w.static_rot = 0.001;
    u64 flips = 0;
    for (u64 t = 0; t < 300; ++t) {
        f.s.set_weather(w);
        f.s.tick(t);
        flips += f.s.tally().static_flips;
    }
    const double ratio = static_cast<double>(flips) / (0.001 * 65536.0 * 300.0);
    CHECK(ratio > 0.95 && ratio < 1.05);
    printf("  rays: storm static flips x%.3f of the weather's rate\n", ratio);
}

// Determinism: rays draw from the node's stream in a fixed order, so the
// same seed gives the same memory, and rays differ from flips.
void test_ray_determinism() {
    u64 sums[3] = {0, 0, 0};
    for (u32 k = 0; k < 3; ++k) {
        RFix f;
        CHECK(f.init(k < 2 ? "hazard.rot = 0.001\n" : "hazard.rot = 0.001\nhazard.rays = false\n", 8192));
        for (u32 b = 0; b < 64; ++b)
            CHECK(f.body(b * 128, 48, 2) != kNone);
        for (u64 t = 0; t < 50; ++t)
            f.s.tick(t);
        for (u32 a = 0; a < 8192; ++a)
            sums[k] = sums[k] * 1099511628211ull + f.s.mem()[a];
    }
    CHECK(sums[0] == sums[1]);
    CHECK(sums[0] != sums[2]);
}

// A busy world with storms (lightning and static) and cosmic rays all as
// rays: the same at 1, 4 and 20 threads and through a snapshot, and not the
// world without rays.
void test_ray_world() {
    const char* storms = "disaster.enabled = true\ndisaster.storm_every = 100\ndisaster.storm_life_min = 100\n"
                         "disaster.storm_life_max = 300\ndisaster.strike_every_min = 10\n"
                         "disaster.strike_every_max = 30\n";
    const u64 ticks = 400;
    const u32 threads[4] = {1, 4, 20, 1};
    u64 hashes[4] = {};
    SoupTally sum = {};
    ByteWriter snap;
    for (u32 v = 0; v < 4; ++v) {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append(storms);
        if (v < 3)
            cfg.append("hazard.rays = true\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            const u64* s = reinterpret_cast<const u64*>(&run.world().last_soup_tick());
            u64* d = reinterpret_cast<u64*>(&sum);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                d[i] += s[i];
            if (t == ticks / 2)
                run.snapshot(snap);
        }
        hashes[v] = run.world().hash();
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK(hashes[3] != hashes[0]);
    CHECK(sum.rot_flips > 0 && sum.strikes > 0 && sum.static_flips > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  rays: %llu cosmic flips, %llu strikes (%llu codons), %llu static flips in %llu ticks; the same at 1, "
           "4 and 20 threads and through a snapshot\n",
           static_cast<unsigned long long>(sum.rot_flips), static_cast<unsigned long long>(sum.strikes),
           static_cast<unsigned long long>(sum.strike_codons), static_cast<unsigned long long>(sum.static_flips),
           static_cast<unsigned long long>(ticks));
}

} // namespace

void suite_lifehistory() {
    test_gestation_cpu();
    test_gestation_per_codon();
    test_gestation_not_frozen();
    test_gestation_warp();
    test_ray_from_outside();
    test_ray_calibration();
    test_ray_shields();
    test_ray_static();
    test_ray_determinism();
    test_ray_world();
}
