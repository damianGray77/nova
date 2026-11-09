// docs/lifehistory-plan.md (revision 2, approved by Damian 2026-11-08):
// gestation (G), ray hazards (R), wipe warnings (W) and voluntary dormancy (V).
#include <cmath>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/hash.h"
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
    // The same from a whole config of its own (no kQuiet).
    bool init_raw(const char* text, u32 size = 1024) {
        if (!make_params(&p, text, size))
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
        extra.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 4\nwarp.lanes_max = 4\n"
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
        cfg.append("hazard.copy = 0\nhazard.flaw = 0\nmem.upkeep = 0\ncost.table = classic\n");
        if (!strstr(extra, "heat.enabled ="))
            cfg.append("heat.enabled = false\n");
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

// --- Ray indels (R7) ------------------------------------------------------------

// The op names of codons [at, at + n).
TextBuf names_at(const Soup& s, u32 at, u32 n) {
    TextBuf t;
    for (u32 a = at; a < at + n; ++a) {
        if (a > at)
            t.append_char(' ');
        t.append(op_name(s.params().table.op[s.mem()[a]]));
    }
    return t;
}

// R7: an insertion copies the codon in place, the codons after it moving up
// one into the free codon past the block's end, which the block claims; a
// deletion moves them back and lets the end go. With the end taken, the
// start is used instead; with neither free, nothing happens (a flip, in a
// ray); a one-codon block isn't shortened. Every invariant holds throughout.
void test_indel_mechanics() {
    LFix f;
    CHECK(f.init("world.millicores = 4000\n"));
    const u32 b = plant(f.s, 100, "INC DEC ZERO ROT PUSH POP SWAP NOT", 500);
    CHECK(b != kNone);
    const u32 k = f.s.body(b).blocks;
    CHECK(f.s.ray_indel(103, true)); // ROT copied in place
    CHECK(names_at(f.s, 100, 9).view().eq("INC DEC ZERO ROT ROT PUSH POP SWAP NOT"));
    CHECK(f.s.block(k).start == 100 && f.s.block(k).len == 9 && f.s.body(b).mem == 9 && f.s.owner_body(108) == b);
    CHECK(soup_ok(f.s));
    CHECK(f.s.ray_indel(101, false)); // DEC deleted
    CHECK(names_at(f.s, 100, 8).view().eq("INC ZERO ROT ROT PUSH POP SWAP NOT"));
    CHECK(f.s.block(k).len == 8 && f.s.body(b).mem == 8 && f.s.owner_body(108) == kNone);
    CHECK(soup_ok(f.s));
    // Its end taken (y right after it): the start is used.
    const u32 x = plant(f.s, 300, "INC DEC ZERO ROT", 500);
    const u32 y = plant(f.s, 304, "NOT NEG", 500);
    CHECK(x != kNone && y != kNone);
    const u32 kx = f.s.body(x).blocks;
    CHECK(f.s.ray_indel(301, true)); // DEC copied, the block grown to 299
    CHECK(f.s.block(kx).start == 299 && f.s.block(kx).len == 5);
    CHECK(names_at(f.s, 299, 5).view().eq("INC DEC DEC ZERO ROT"));
    CHECK(f.s.ray_indel(301, false)); // a DEC deleted, 299 let go
    CHECK(f.s.block(kx).start == 300 && f.s.block(kx).len == 4);
    CHECK(names_at(f.s, 300, 4).view().eq("INC DEC ZERO ROT"));
    CHECK(soup_ok(f.s));
    // Hemmed in on both sides: nothing, either way.
    const u32 w = plant(f.s, 296, "INC INC INC INC", 500); // [296, 300), x at [300, 304), y at [304, 306)
    CHECK(w != kNone);
    CHECK(!f.s.ray_indel(302, true) && !f.s.ray_indel(302, false));
    CHECK(f.s.block(kx).len == 4 && names_at(f.s, 300, 4).view().eq("INC DEC ZERO ROT"));
    // A one-codon block isn't shortened (it may grow).
    const u32 one = plant(f.s, 500, "NOT", 500);
    CHECK(one != kNone && !f.s.ray_indel(500, false) && f.s.ray_indel(500, true) && f.s.body(one).mem == 2);
    CHECK(soup_ok(f.s));
    printf("  ray indels: a codon copied in place or deleted, the block growing or shrinking at a free end\n");
}

// R7: a copier that copies its own size passes an indel on: the budder
// (SENSE: my size; ALLOC it; DIVIDE) with its SENSE copied in place senses 4
// and divides off a daughter of 4.
void test_indel_inherits() {
    LFix f;
    CHECK(f.init(""));
    const u32 b = plant(f.s, 100, kBudder, 1000);
    CHECK(b != kNone && f.s.ray_indel(100, true));
    CHECK(names_at(f.s, 100, 4).view().eq("SENSE SENSE ALLOC DIVIDE"));
    const u64 t = f.first_birth(0, 10);
    CHECK(t != ~0ull);
    u32 daughter_len = 0;
    for (usize i = 0; i < f.s.body_count(); ++i) {
        const u32 d = f.s.body_at(i);
        if (d != b && f.s.body(d).blocks != kNone)
            daughter_len = f.s.block(f.s.body(d).blocks).len;
    }
    CHECK_EQ_U64(daughter_len, 4);
}

// R7 through the tick: energetic cosmic hits on living code make insertions
// and deletions about equally often (each needs a free end), every invariant
// holds, and the same seed gives the same world.
void test_indel_rays() {
    u64 hashes[2] = {0, 0}, ins = 0, del = 0;
    for (u32 v = 0; v < 2; ++v) {
        RFix f;
        CHECK(f.init("hazard.rot = 0.01\nhazard.ray_flip = 0.25\nhazard.ray_indel = 1\nhazard.ray_indel_energy = 0\n"
                     "world.millicores = 4000\n",
                     8192));
        for (u32 i = 0; i < 64; ++i)
            CHECK(f.body(i * 128 + 32, 32, 0) != kNone);
        u64 in = 0, out = 0;
        for (u64 t = 0; t < 40; ++t) {
            f.s.tick(t);
            in += f.s.tally().ray_inserts;
            out += f.s.tally().ray_deletes;
        }
        CHECK(soup_ok(f.s));
        for (u32 a = 0; a < 8192; ++a)
            hashes[v] = hashes[v] * 1099511628211ull + f.s.mem()[a] + 7ull * f.s.owner_body(a);
        if (v == 0) {
            ins = in;
            del = out;
        }
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK(ins > 50 && del > 50);
    const double ratio = static_cast<double>(ins) / static_cast<double>(del);
    CHECK(ratio > 0.7 && ratio < 1.4);
    printf("  ray indels: %llu insertions and %llu deletions in 40 ticks (x%.2f); the same world twice\n",
           static_cast<unsigned long long>(ins), static_cast<unsigned long long>(del), ratio);
}

// --- Cheaper corpses (2026-11-08) ---------------------------------------------

// A timed world where a blind bite of 16 codons of waste beside the eater
// lands: of a dead program's fresh remnant (`corpse`, stamped when it died)
// or of raw waste. Returns the bite's ticks; *eaten its corpse codons.
u64 corpse_bite(bool corpse, double cost, u64* eaten) {
    TextBuf cfg;
    cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
               "cost.table = home_v1\nengulf.bite = true\nengulf.codons_per_tick = 8\nsalvage.ratio = 4\n"
               "engulf.bite_share = 1\n" // (bites up to the eater's size: the cap isn't what this measures)
               "world.millicores = 4000\n");
    char line[64];
    snprintf(line, sizeof line, "engulf.corpse_cost = %g\n", cost);
    cfg.append(line);
    LFix f;
    *eaten = 0;
    if (!f.init_raw(cfg.c_str()))
        return 0;
    TextBuf code;
    for (int i = 0; i < 16; ++i)
        code.append(i ? " INC" : "INC");
    if (corpse) { // a 16-codon program at [84, 100) dies: its code is left, stamped
        const u32 v = plant(f.s, 84, code.c_str(), 100);
        f.s.cpu_mut(f.s.body(v).cpus).halted = 1;
        f.s.death_sweep();
        if (f.s.body(v).alive || f.s.stamp(90) == 0)
            return 0;
    }
    TextBuf eat;
    eat.append("ENGULF");
    for (int i = 0; i < 15; ++i)
        eat.append(" INC");
    const u32 e = plant(f.s, 100, eat.c_str(), 500);
    f.s.cpu_mut(f.s.body(e).cpus).cx = 16;
    f.s.run_cpu(f.s.body(e).cpus, 1000000);
    if (f.s.actions().size() != 1 || f.s.actions()[0].target != kNone)
        return 0;
    const u64 ticks = f.s.actions()[0].end;
    f.s.begin_tick(ticks);
    f.s.run_cpu(f.s.body(e).cpus, 0);
    *eaten = f.s.tally().corpse_codons;
    return f.s.tally().bites_waste == 1 && f.s.owner_body(84) == e && soup_ok(f.s) ? ticks : 0;
}

// The same world, a blind bite (no template: a dormant program has no
// surface to aim at) from an eater's last codon at a 16-codon program right
// after it, nearer than any waste: `dormant` (resting on a memory reserve)
// or awake. Returns the bite's ticks; *eaten its discounted codons.
u64 dormant_bite(bool dormant, double cost, u64* eaten) {
    TextBuf cfg;
    cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
               "cost.table = home_v1\nengulf.bite = true\nengulf.codons_per_tick = 8\nsalvage.ratio = 4\n"
               "engulf.bite_share = 1\n" // (bites up to the eater's size: the cap isn't what this measures)
               "world.millicores = 4000\nlife.dormancy = true\n");
    char line[64];
    snprintf(line, sizeof line, "engulf.corpse_cost = %g\n", cost);
    cfg.append(line);
    LFix f;
    *eaten = 0;
    if (!f.init_raw(cfg.c_str()))
        return 0;
    TextBuf eat;
    for (int i = 0; i < 15; ++i)
        eat.append(i ? " INC" : "INC");
    eat.append(" ENGULF"); // at 115
    const u32 e = plant(f.s, 100, eat.c_str(), 500);
    TextBuf prey_code;
    prey_code.append("INC ALLOC YIELD"); // (no surface to shield it awake; the YIELD ends its turn after the ALLOC)
    for (int i = 0; i < 13; ++i)
        prey_code.append(" INC");
    const u32 v = plant(f.s, 116, prey_code.c_str(), 100);
    Cpu& pc = f.s.cpu_mut(f.s.body(v).cpus);
    pc.ip = 117; // its ALLOC: a reserve of 8
    pc.bx = 0;
    pc.cx = 8;
    f.s.run_cpu(f.s.body(v).cpus, 1000000);
    if (f.s.body(v).mem != 24 || (dormant && !(f.s.force_dormant(v) && f.s.dormant(v))))
        return 0;
    Cpu& ec = f.s.cpu_mut(f.s.body(e).cpus);
    ec.ip = 115;
    ec.cx = 16;
    f.s.run_cpu(f.s.body(e).cpus, 1000000);
    if (f.s.actions().size() != 1 || f.s.actions()[0].target != v)
        return 0;
    const u64 ticks = f.s.actions()[0].end;
    f.s.begin_tick(ticks);
    f.s.run_cpu(f.s.body(e).cpus, 0);
    *eaten = f.s.tally().corpse_codons;
    const bool landed = f.s.tally().bites_body + f.s.tally().engulfs == 1;
    f.s.death_sweep(); // (an eaten sleeper is cleared there, as in suite ecology's)
    return landed && soup_ok(f.s) ? ticks : 0;
}

// Cheaper corpses: a bite of a fresh corpse, or of a dormant program, takes
// engulf.corpse_cost of the time a bite of raw waste or of a waking program
// does (16 codons at 8 a tick: 1 tick at 0.5, 2 otherwise); at 1 they cost
// the same, as before.
void test_corpse_bites() {
    u64 eaten = 0;
    CHECK_EQ_U64(corpse_bite(false, 0.5, &eaten), 2);
    CHECK_EQ_U64(eaten, 0);
    CHECK_EQ_U64(corpse_bite(true, 0.5, &eaten), 1);
    CHECK_EQ_U64(eaten, 16);
    CHECK_EQ_U64(corpse_bite(true, 1.0, &eaten), 2);
    CHECK_EQ_U64(eaten, 16);
    CHECK_EQ_U64(dormant_bite(true, 0.5, &eaten), 1);
    CHECK_EQ_U64(eaten, 16);
    CHECK_EQ_U64(dormant_bite(false, 0.5, &eaten), 2); // awake: no discount (it bites 16 of its 24)
    CHECK_EQ_U64(eaten, 0);
    printf("  corpses: a bite of 16 codons of a fresh corpse or a dormant program takes 1 tick, of raw waste or a "
           "waking program 2\n");
}

// --- Wipe warnings (W) --------------------------------------------------------

const char* const kWipes = "hazard.rays = false\nmem.wipe_pages = 1\n";

// W2: a wipe lands exactly mem.wipe_warning ticks after it's drawn, and its
// page is untouched until then (no flicker here).
void test_wipe_lands_late() {
    RFix f;
    TextBuf extra;
    extra.append(kWipes);
    extra.append("mem.volatility = 0.002\nmem.wipe_warning = 10\nmem.wipe_false_alarms = 0\nmem.wipe_flicker = 0\n");
    CHECK(f.init(extra.c_str(), 4096));
    u64 t = 0;
    while (f.s.pending_wipes().empty() && t < 1000)
        f.s.tick(t++);
    CHECK(!f.s.pending_wipes().empty());
    if (f.s.pending_wipes().empty())
        return;
    const PendingWipe x = f.s.pending_wipes()[0];
    CHECK(x.drawn == t - 1 && x.lands == x.drawn + 10 && x.pages == 1 && !x.false_alarm);
    f.p.volatility = 0; // no more draws: the ones pending still land
    const u32 lo = x.page * kPage;
    u64 wipes = 0;
    for (; t < x.lands; ++t) {
        f.s.tick(t);
        wipes += f.s.tally().wipes;
    }
    CHECK(wipes == 0 && f.changed(lo, kPage) == 0);
    f.s.tick(t); // the landing tick
    CHECK(f.s.tally().wipes >= 1 && f.changed(lo, kPage) > kPage / 2);
    CHECK(f.s.pending_wipes().empty());
    printf("  wipe warnings: drawn at tick %llu, page %u untouched until it landed at tick %llu\n",
           static_cast<unsigned long long>(x.drawn), x.page, static_cast<unsigned long long>(x.lands));
}

// W2: the symptoms: the doomed page heats toward the memory curve's hot mark
// and flickers more and more, and nothing else does.
void test_wipe_symptoms() {
    RFix f;
    TextBuf extra;
    extra.append(kWipes);
    extra.append("heat.enabled = true\nmem.volatility = 0.002\nmem.wipe_warning = 20\nmem.wipe_false_alarms = 0\n"
                 "mem.wipe_flicker = 0.05\n");
    CHECK(f.init(extra.c_str(), 4096));
    u64 t = 0;
    while (f.s.pending_wipes().empty() && t < 1000)
        f.s.tick(t++);
    CHECK(f.s.pending_wipes().size() == 1);
    if (f.s.pending_wipes().size() != 1)
        return;
    const PendingWipe x = f.s.pending_wipes()[0];
    f.p.volatility = 0;
    const u32 pages = 4096 / kPage;
    u64 early = 0, late = 0;
    bool ramp = true, others_cool = true;
    float last = 0;
    for (; t < x.lands; ++t) {
        f.s.tick(t);
        const float h = f.s.page_heat(x.page);
        const float want = static_cast<float>(t - x.drawn) / 20.0f * f.p.mem.hot;
        ramp = ramp && h >= want - 0.001f && h >= last;
        last = h;
        for (u32 q = 0; q < pages; ++q)
            others_cool = others_cool && (q == x.page || f.s.page_heat(q) == 0);
        (t - x.drawn < 10 ? early : late) += f.s.tally().wipe_flickers;
    }
    const u32 lo = x.page * kPage;
    CHECK(ramp && others_cool && last >= 0.9f * f.p.mem.hot && last < f.p.mem.max);
    CHECK(late > early && early + late > 10);
    CHECK(f.changed(lo, kPage) > 0 && f.changed(0, lo) == 0 && f.changed(lo + kPage, 4096 - lo - kPage) == 0);
    printf("  wipe warnings: the doomed page heats to %.1f (hot %.0f) and flickers %llu then %llu times; no other "
           "page changes\n",
           last, f.p.mem.hot, static_cast<unsigned long long>(early), static_cast<unsigned long long>(late));
}

// W3: the false-alarm share holds, and every warning that isn't one lands.
void test_wipe_false_alarms() {
    RFix f;
    TextBuf extra;
    extra.append(kWipes);
    extra.append("mem.volatility = 0.005\nmem.wipe_warning = 5\n");
    CHECK(f.init(extra.c_str(), 65536));
    u64 warned = 0, fa = 0, wipes = 0, t = 0;
    for (; t < 200; ++t) {
        f.s.tick(t);
        warned += f.s.tally().wipe_warnings;
        fa += f.s.tally().wipe_false_alarms;
        wipes += f.s.tally().wipes;
    }
    f.p.volatility = 0;
    for (; t < 206; ++t) {
        f.s.tick(t);
        wipes += f.s.tally().wipes;
    }
    CHECK(f.s.pending_wipes().empty());
    const double share = static_cast<double>(fa) / static_cast<double>(warned);
    CHECK(warned > 500 && share > 0.2 && share < 0.3);
    CHECK_EQ_U64(wipes, warned - fa);
    printf("  wipe warnings: %llu warnings, %.3f false alarms (mem.wipe_false_alarms 0.25), the rest landed\n",
           static_cast<unsigned long long>(warned), share);
}

// W5: a busy world with wipe warnings is the same at 1, 4 and 20 threads and
// through a snapshot taken with wipes pending, and not the world without
// them; a v4 snapshot (no warnings) still loads and resumes exactly.
void test_wipe_world() {
    const char* wipes = "mem.volatility = 0.0005\nmem.wipe_warning = 30\nheat.enabled = true\n";
    const u64 ticks = 300;
    const u32 threads[4] = {1, 4, 20, 1};
    u64 hashes[4] = {};
    ByteWriter snap;
    u64 pending_at_snap = 0, warnings = 0;
    for (u32 v = 0; v < 4; ++v) {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append(v < 3 ? wipes : "mem.volatility = 0.0005\nheat.enabled = true\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            warnings += run.world().last_soup_tick().wipe_warnings;
            if (t == ticks / 2) {
                run.snapshot(snap);
                for (u32 n = 0; n < run.world().node_count(); ++n)
                    pending_at_snap += run.world().node_soup(n).pending_wipes().size();
            }
        }
        hashes[v] = run.world().hash();
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK(hashes[3] != hashes[0]);
    CHECK(warnings > 0 && pending_at_snap > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  wipe warnings: %llu in %llu ticks, %llu pending at the snapshot; the same at 1, 4 and 20 threads, "
           "and through the snapshot\n",
           static_cast<unsigned long long>(warnings), static_cast<unsigned long long>(ticks),
           static_cast<unsigned long long>(pending_at_snap));
}

// --- Voluntary dormancy (V) --------------------------------------------------

// A body at 100 that ALLOCs a memory reserve of `reserve` codons (BX 0), then
// runs YIELD with BX = sel and CX = n; then the death sweep.
u32 sleeper(LFix& f, u32 reserve, u32 sel, u32 n, u32 units = 1000) {
    const u32 b = plant(f.s, 100, "ALLOC YIELD INC INC", units);
    if (b == kNone)
        return kNone;
    const u32 c = f.s.body(b).cpus;
    f.s.cpu_mut(c).bx = 0;
    f.s.cpu_mut(c).cx = reserve;
    if (reserve)
        f.s.run_cpu(c, 1);
    else
        f.s.cpu_mut(c).ip = 101; // (no ALLOC: straight to the YIELD)
    f.s.cpu_mut(c).bx = sel;
    f.s.cpu_mut(c).cx = n;
    f.s.run_cpu(c, 1);
    f.s.death_sweep();
    return b;
}

// V1-V3: YIELD with BX 1 and CX 10 sleeps 10 ticks where it is, on its
// reserve, and wakes then, not before -- though its own cores were free all
// along -- with its cores again.
void test_sleep_wakes() {
    LFix f;
    CHECK(f.init("life.sleep = true\nworld.millicores = 2000\n"));
    const u32 b = sleeper(f, 64, 1, 10);
    CHECK(b != kNone && f.s.dormant(b) && f.s.tally().sleeps == 1 && f.s.body(b).units == 0);
    const Dormant& d = f.s.dormant_state(b);
    CHECK((d.flags & kDormantVoluntary) && (d.flags & kDormantStays) && d.spare == 10 && d.mem_reserve == 64);
    bool slept = true;
    for (u64 t = 1; t < 10; ++t) {
        f.s.begin_tick(t);
        f.s.death_sweep();
        slept = slept && f.s.dormant(b);
    }
    CHECK(slept && f.s.dormant_state(b).mem_reserve == 64 - 9);
    f.s.begin_tick(10);
    f.s.death_sweep();
    CHECK(!f.s.dormant(b) && f.s.body(b).alive && f.s.body(b).units == 1000 && f.s.tally().sleep_woke == 1);
    CHECK(soup_ok(f.s));
    // CX above dormant.sleep_max is cut to it; CX 0, or another BX, is a plain YIELD.
    LFix g;
    CHECK(g.init("life.sleep = true\ndormant.sleep_max = 7\n"));
    const u32 c = sleeper(g, 16, 1, 1000000);
    CHECK(g.s.dormant(c) && g.s.dormant_state(c).spare == 7);
    for (u32 v = 0; v < 2; ++v) {
        LFix h;
        CHECK(h.init("life.sleep = true\n"));
        const u32 x = sleeper(h, 16, v ? 3 : 1, v ? 10 : 0);
        CHECK(h.s.body(x).alive && !h.s.dormant(x) && h.s.tally().sleeps == 0);
    }
    printf("  sleep: YIELD BX 1 CX 10 slept 10 ticks on its reserve with its cores free, then woke with them\n");
}

// V2: a sleep without a reserve dies (it has nothing to sleep on); BX 2
// drifts, its timer going with it; with life.sleep off YIELD is as before.
void test_sleep_bare_drift_off() {
    LFix f;
    CHECK(f.init("life.sleep = true\n"));
    const u32 b = sleeper(f, 0, 1, 5);
    CHECK(!f.s.body(b).alive && f.s.tally().sleeps == 1 && f.s.tally().sleep_expired == 1);
    LFix g;
    CHECK(g.init("life.sleep = true\n"));
    const u32 edges[1] = {0};
    const u32 in_flight[1] = {0};
    DispersalPorts ports;
    ports.edges = edges;
    ports.in_flight = in_flight;
    ports.degree = 1;
    ports.capacity = 2;
    g.s.set_ports(ports);
    const u32 d = sleeper(g, 32, 2, 12);
    CHECK(!g.s.body(d).alive && g.s.departures().size() == 1);
    if (g.s.departures().size() == 1) {
        const Departure& dep = g.s.departures()[0];
        CHECK(dep.dormant == 1 && dep.sleep == ((static_cast<u32>(kDormantVoluntary) << 16) | 12u) &&
              dep.mem_reserve == 32);
    }
    LFix h;
    CHECK(h.init(""));
    const u32 x = sleeper(h, 16, 1, 10);
    CHECK(h.s.body(x).alive && !h.s.dormant(x) && h.s.tally().sleeps == 0);
}

// V4: life.dormancy (the automatic rule) and life.sleep (the instruction)
// apart: with only the instruction a frozen body dies, as before dormancy;
// with both it goes dormant; and the instruction works either way.
void test_sleep_modes() {
    for (u32 m = 0; m < 4; ++m) {
        const bool autom = (m & 1) != 0, sleep = (m & 2) != 0;
        TextBuf cfg;
        cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 1000000\n"
                   "world.millicores = 2000\nworld.disk = 4096\nlife.frozen_ticks = 5\ncost.table = classic\n");
        cfg.append(autom ? "life.dormancy = true\n" : "life.dormancy = false\n");
        cfg.append(sleep ? "life.sleep = true\n" : "life.sleep = false\n");
        LFix f;
        CHECK(f.init_raw(cfg.c_str()));
        const u32 b = plant(f.s, 100, "ALLOC INC INC INC", 1000);
        Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
        c.bx = 3; // a disk reserve
        c.cx = 16;
        f.s.run_cpu(f.s.body(b).cpus, 1);
        for (u64 t = 0; t <= 5; ++t)
            f.s.tick(t);
        CHECK(autom ? (f.s.body(b).alive && f.s.dormant(b)) : !f.s.body(b).alive);
    }
}

// V5: a warp's YIELD with BX 1 and CX > 0 (the first lane's) is a host
// instruction with life.sleep on, and sends the GPU-born body to sleep.
void test_sleep_warp() {
    LFix f;
    CHECK(f.init("life.sleep = true\ndormant.sleep_max = 8\ndormant.mem_loss = 0\nworld.millicores = 1000\n"
                 "world.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 4\nwarp.lanes_max = 4\nwarp.step_cost = 1\nwarp.lane_cost = 0\n"
                 "warp.capacity = 50\n"));
    // CX 4, ALLOC a reserve of 4; then BX 1 (ROTB: CX into BX) and CX 5.
    Array<u8> code;
    TextBuf err;
    CHECK(genome_from_names(f.p.table, "INC INC INC INC ALLOC ZERO INC ROTB INC INC INC INC INC YIELD INC INC",
                            &code, &err));
    for (usize i = 0; i < code.size(); ++i)
        f.s.mem_mut()[100 + i] = code[i];
    const u32 b = f.s.create_body_form(100, static_cast<u32>(code.size()), 0, 1, kFormGpu);
    CHECK(b != kNone);
    u64 t = 0, sleeps = 0;
    for (; t < 30 && !f.s.dormant(b); ++t) {
        f.s.tick(t);
        sleeps += f.s.tally().sleeps;
    }
    CHECK(f.s.dormant(b) && sleeps == 1);
    if (f.s.dormant(b)) {
        const Dormant& d = f.s.dormant_state(b);
        CHECK((d.flags & kDormantVoluntary) && (d.flags & kDormantStays) && d.spare == 5 && d.form == kFormGpu);
        CHECK(d.gpus == 1 && f.s.warp_count(b) == 0);
    }
    printf("  sleep: a warp's YIELD went to the host, which sent its GPU-born body to sleep at tick %llu\n",
           static_cast<unsigned long long>(t - 1));
}

// A busy warp world with sleep on (random code yields with BX 1 or 2 and CX
// > 0 often): the same at 1, 4 and 20 threads and through a snapshot, not the
// world without sleep; and on the real GPU, every round verified against
// the reference, the reference's world.
const char* sleep_world(bool sleep) {
    static TextBuf t[2];
    TextBuf& w = t[sleep ? 1 : 0];
    if (w.size())
        return w.c_str();
    w.append("seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 16\nworld.soup_size = 2048\n"
             "world.millicores = 300\nworld.gpus = 300\ngpu.warps = true\n"
             "edge.bandwidth = 8\ncpu.instr_per_core = 2500\ncpu.wear = 0.002\ngpu.wear = 0.002\n"
             "spark.timing = last\nspark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.gpu_grant = 6\n"
             "spark.min_len = 8\nspark.max_len = 32\nhazard.copy = 0.005\nhazard.flaw = 0.001\n"
             "hazard.rot = 0.00005\nworld.disk = 512\nlife.dormancy = true\nlife.frozen_ticks = 40\n"
             "seed.at = 600\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 64\nseed.genome = "
             "SENSE ALLOC SHR SHR SHR SHR SHR NOP1 COPY DJNZ NOP0 ZERO INC INC ROTB "
             "ZERO INC SHL SHL SHL ALLOC ZERO ROTB INC SHL SHL SHL SHL SHL DIVIDE ZERO ZERO\n");
    if (sleep) // (short sleeps, so some end within the run)
        w.append("life.sleep = true\ndormant.sleep_max = 20\n");
    return w.c_str();
}

void test_sleep_world() {
    const u64 ticks = 200;
    const u32 threads[4] = {1, 4, 20, 1};
    u64 hashes[4] = {};
    SoupTally sum = {};
    ByteWriter snap;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, sleep_world(v < 3), threads[v]));
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
    CHECK(sum.sleeps > 0); // (random code mostly sleeps with no reserve, and dies: the unit tests wake)
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  sleep: %llu sleeps (%llu woke, %llu expired) in %llu ticks of a busy warp world; the same at 1, 4 and "
           "20 threads and through a snapshot\n",
           static_cast<unsigned long long>(sum.sleeps), static_cast<unsigned long long>(sum.sleep_woke),
           static_cast<unsigned long long>(sum.sleep_expired), static_cast<unsigned long long>(ticks));
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < n; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    if (!gpu) {
        printf("  sleep: no hardware GPU, the device check skipped\n");
        return;
    }
    TextBuf cfg;
    cfg.append(sleep_world(true));
    cfg.append("warp.device = gpu\nwarp.verify = 1\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    run.run_until(ticks);
    const World& w = run.world();
    CHECK(w.warp_gpu_rounds() > 0 && w.warp_verified() == w.warp_gpu_rounds() && w.warp_mismatches() == 0);
    CHECK_EQ_U64(w.hash(), hashes[0]);
    printf("  sleep: on the GPU (%s), %llu rounds verified, %llu differed; the reference's world\n", w.warp_kernel(),
           static_cast<unsigned long long>(w.warp_verified()), static_cast<unsigned long long>(w.warp_mismatches()));
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
    test_indel_mechanics();
    test_indel_inherits();
    test_indel_rays();
    test_corpse_bites();
    test_wipe_lands_late();
    test_wipe_symptoms();
    test_wipe_false_alarms();
    test_wipe_world();
    test_sleep_wakes();
    test_sleep_bare_drift_off();
    test_sleep_modes();
    test_sleep_warp();
    test_sleep_world();
}
