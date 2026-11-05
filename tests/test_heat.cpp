// Heat and hazards (design.md 4.5, 6.3, 6.4 #2, #17, #18): curve shapes and
// staggered onsets, throttle-then-release, freed units keeping their heat, and
// measured hazard rates against their configured values.
#include <cmath>

#include "check.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// Heat on, hazards off unless a test turns one on.
const char* const kHeat = "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n";

struct Fix {
    SoupParams p;
    Soup s;
    // hazards_off prepends kHeat; hazard tests pass all three rates themselves.
    bool init(const char* extra, u32 size = 4096, bool hazards_off = true) {
        TextBuf cfg;
        if (hazards_off)
            cfg.append(kHeat);
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 17, 0);
        return true;
    }
    void heat_body(u32 b, float h) {
        for (u32 u = 0; u < p.millicores; ++u)
            if (s.unit_owner(u) == b)
                s.set_unit_heat(u, h);
    }
    double mean_heat(u32 b) const {
        double sum = 0;
        u32 n = 0;
        for (u32 u = 0; u < p.millicores; ++u)
            if (s.unit_owner(u) == b) {
                sum += s.unit_heat(u);
                ++n;
            }
        return n ? sum / n : 0;
    }
};

bool near(double got, double want, double tol) {
    bool ok = fabs(got - want) <= tol;
    if (!ok)
        fprintf(stderr, "  near: got %g, want %g +- %g\n", got, want, tol);
    return ok;
}

void curves() {
    HeatCurve c = {40, 60, 100, 0.25f, 10, 0};
    CHECK(c.speed(0) == 1 && c.speed(60) == 1 && c.speed(100) == 0.25f && c.speed(500) == 0.25f);
    CHECK(c.err_mult(40) == 1 && c.err_mult(100) == 10);
    bool mono = true;
    for (float h = 0; h < 120; h += 0.5f)
        mono = mono && c.speed(h + 0.5f) <= c.speed(h) && c.err_mult(h + 0.5f) >= c.err_mult(h);
    CHECK(mono);
    // Staggered: between warm and hot, errors already rise at full speed (the
    // "fast but sloppy" overclocking zone).
    CHECK(c.err_mult(50) > 1 && c.speed(50) == 1);
    CHECK(c.speed(80) == 0.625f);

    SoupParams p;
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse("heat.cpu.warm = 70\nheat.cpu.hot = 60\n", &pe));
    CHECK(!soup_params_from_config(cfg, 1024, &p));
    CHECK(Str(cfg.value_error()).starts_with("config: heat.cpu.warm: onsets must be staggered"));
}

// A flat-out body heats to the point where throttling balances cooling; a body
// that rests stays cool. Throttling really cuts the instruction budget.
void throttle() {
    Fix f;
    CHECK(f.init("world.millicores = 2000\n"));
    u32 busy = plant(f.s, 100, "INC", 1000);
    u32 lazy = plant(f.s, 200, "YIELD", 1000);
    for (u64 t = 0; t < 200; ++t)
        f.s.tick(t);
    // Default gain 6 and cooling 0.1: equilibrium where 6 * speed(h) = 0.1 h,
    // which with hot = 60 sits right at the throttle onset.
    CHECK(near(f.mean_heat(busy), 60, 1.5));
    CHECK(f.mean_heat(lazy) < 0.1);

    Fix g;
    CHECK(g.init("world.millicores = 2000\n"));
    u32 b = plant(g.s, 100, "INC", 1000);
    g.heat_body(b, 80); // speed(80) = 0.625
    g.s.tick(0);
    CHECK(g.s.cpu(g.s.body(b).cpus).executed == 625);
    CHECK(soup_ok(g.s));
}

// Past max a unit is freed, and keeps its heat; it then cools at the normal
// rate. RELEASE then ALLOC cannot launder heat.
void release_keeps_heat() {
    Fix f;
    CHECK(f.init("world.millicores = 100\n"));
    u32 b = plant(f.s, 100, "INC", 100);
    f.heat_body(b, 150);
    f.s.tick(0);
    CHECK(f.s.body_count() == 0); // every unit overheated: no CPU, dead
    CHECK(f.s.free_units() == 100 && f.s.tally().cpu_released == 100);
    double h0 = f.s.unit_heat(0);
    CHECK(h0 > 100); // 150 * 0.9 + work: still hot in the pool
    f.s.tick(1);
    CHECK(near(f.s.unit_heat(0), h0 * 0.9, 1e-3));

    Fix g;
    CHECK(g.init("world.millicores = 100\n"));
    u32 c = plant(g.s, 100, "RELEASE ALLOC", 100);
    g.heat_body(c, 90);
    g.s.begin_tick(0);
    Cpu& cpu = g.s.cpu_mut(g.s.body(c).cpus);
    cpu.bx = 1;
    cpu.cx = 100;
    steps(g.s, c, 2);
    CHECK(g.s.body(c).units == 100 && g.s.cpu(g.s.body(c).cpus).err == 0);
    CHECK(near(g.mean_heat(c), 90, 1e-6)); // the same hot units came back
}

// Hot memory: fetches and writes heat pages; a page past max is freed under its
// owner, turning its codons to waste; a hot page refuses writes.
void memory_heat() {
    Fix f;
    CHECK(f.init("world.millicores = 1000\nheat.fetch_gain = 0.01\n"));
    u32 b = plant(f.s, 128, "INC", 1000);
    for (u64 t = 0; t < 100; ++t)
        f.s.tick(t);
    CHECK(near(f.s.page_heat(2), 100.0, 1.0)); // 1000 fetches x 0.01 / 0.1
    CHECK(f.s.page_heat(3) == 0);
    (void)b;

    Fix g;
    CHECK(g.init("world.millicores = 1000\n"));
    u32 big = plant(g.s, 60, "INC INC INC INC INC INC INC INC INC INC", 500);
    g.s.set_page_heat(1, 150); // codons 64-127: six of the ten
    g.s.tick(0);
    CHECK(g.s.tally().mem_released == 6);
    CHECK(g.s.body(big).mem == 4 && g.s.owner_body(64) == kNone && g.s.owner_body(63) == big);
    CHECK(soup_ok(g.s));

    Fix h; // writes into a page at 85 (mem: hot 70, max 100) pass half the time
    CHECK(h.init("world.millicores = 1000\nheat.write_gain = 0\n"));
    u32 w = plant(h.s, 100, "ALLOC COPY", 1000);
    h.s.begin_tick(0);
    Cpu& cpu = h.s.cpu_mut(h.s.body(w).cpus);
    cpu.cx = 1000;
    steps(h.s, w, 1);                                // ALLOC [102, 1102)
    h.s.mem_mut()[100] = h.s.mem()[101];             // now COPY COPY
    h.s.cpu_mut(h.s.body(w).cpus).rh = 102;          // read and write inside it
    h.s.cpu_mut(h.s.body(w).cpus).wh = 640;
    for (u32 pg = 10; pg < 16; ++pg)                 // 640-1023 hot
        h.s.set_page_heat(pg, 85);
    u32 ok = 0, tries = 0;
    for (u32 i = 0; i < 4000; ++i) {
        if (h.s.cpu(h.s.body(w).cpus).wh >= 1024) { // keep writing into the hot pages
            h.s.cpu_mut(h.s.body(w).cpus).wh = 640;
            h.s.cpu_mut(h.s.body(w).cpus).rh = 102;
        }
        h.s.cpu_mut(h.s.body(w).cpus).errs = 0; // a refused-write streak must not halt it
        steps(h.s, w, 1);
        ++tries;
        ok += h.s.cpu(h.s.body(w).cpus).err == 0;
    }
    // speed(85) = 1 - 0.75 * 15/30 = 0.625 of writes go through (5 sigma: 0.04).
    CHECK(near(static_cast<double>(ok) / tries, 0.625, 0.04));
}

// Measures one hazard: n events expected, tolerance 5 sigma (Poisson).
bool rate_ok(u64 got, double expected) {
    return near(static_cast<double>(got), expected, 5 * sqrt(expected) + 1);
}

void hazards() {
    // Copy errors per codon copied, cold and hot.
    for (int hot = 0; hot < 2; ++hot) {
        Fix f;
        CHECK(f.init("world.millicores = 1000\nhazard.copy = 0.002\nhazard.flaw = 0\n"
                     "hazard.rot = 0\ncpu.instr_per_core = 100000\n",
                     4096, false));
        u32 b = plant(f.s, 100, "ALLOC COPY", 1000);
        f.s.begin_tick(0);
        f.s.cpu_mut(f.s.body(b).cpus).cx = 1000;
        steps(f.s, b, 1);
        f.s.mem_mut()[100] = f.s.mem()[101]; // COPY COPY, reading and writing its block
        f.s.cpu_mut(f.s.body(b).cpus).rh = 102;
        f.s.cpu_mut(f.s.body(b).cpus).wh = 602;
        f.heat_body(b, hot ? 70.0f : 0.0f); // err_mult(70) = 1 + 9 * 30/60 = 5.5
        f.s.tick(1);
        u64 copies = f.s.tally().instructions;
        double expected = copies * 0.002 * (hot ? 5.5 : 1.0);
        CHECK(rate_ok(f.s.tally().copy_errors, expected));
    }
    { // execution flaws per instruction
        Fix f;
        CHECK(f.init("world.millicores = 1000\nhazard.copy = 0\nhazard.flaw = 0.001\n"
                     "hazard.rot = 0\ncpu.instr_per_core = 200000\n",
                     4096, false));
        plant(f.s, 100, "NOP0", 1000);
        f.s.tick(0);
        CHECK(rate_ok(f.s.tally().flaws, 200000 * 0.001));
    }
    { // bit rot per codon per tick, cold and with every page hot
        Fix f;
        CHECK(f.init("hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0.001\n", 4096, false));
        u64 cold = 0, warm = 0;
        for (u64 t = 0; t < 100; ++t) {
            f.s.tick(t);
            cold += f.s.tally().rot_flips;
        }
        for (u64 t = 100; t < 200; ++t) {
            for (u32 pg = 0; pg < 4096 / kPage; ++pg)
                f.s.set_page_heat(pg, 70);
            f.s.tick(t);
            warm += f.s.tally().rot_flips;
        }
        // Rot runs after the tick's heat update, so it sees 70 cooled once.
        double m = f.p.mem.err_mult(70.0f * (1.0f - f.p.cool));
        CHECK(rate_ok(cold, 100 * 4096 * 0.001));
        CHECK(m > 4 && rate_ok(warm, 100 * 4096 * 0.001 * m));
    }
}

} // namespace

void suite_heat() {
    curves();
    throttle();
    release_keeps_heat();
    memory_heat();
    hazards();
}
