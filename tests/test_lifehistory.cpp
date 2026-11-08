// docs/lifehistory-plan.md (revision 2, approved by Damian 2026-11-08):
// gestation (G), ray hazards (R), wipe warnings (W) and voluntary dormancy (V).
#include <cstring>

#include "check.h"
#include "helpers.h"
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

} // namespace

void suite_lifehistory() {
    test_gestation_cpu();
    test_gestation_per_codon();
    test_gestation_not_frozen();
    test_gestation_warp();
}
