// Phase 1b rules (design.md 6.6): memory upkeep (D1), CPU wear and spark
// timing (D10), live spark knobs (D4), and the pinned hashes -- Phase 1a's at
// mem.upkeep = 0 and the new default's.
#include <cmath>

#include "check.h"
#include "helpers.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// The busy soup world (helpers.h) at tick 300 under the Phase 1b defaults.
constexpr u64 kPinnedDefault = 0xa384722ce1cbc5e0ull;

// Heat and hazards off; upkeep left to each test.
const char* const kCalm = "heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n";

struct Fix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 4096, u64 seed = 3) {
        TextBuf cfg;
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, seed, 0);
        return true;
    }
};

// n copies of an op name, space-separated.
void repeat(TextBuf& out, const char* op, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (i)
            out.append_char(' ');
        out.append(op);
    }
}

// Upkeep is paid before the body runs, out of its budget, and is counted as work.
void upkeep_arithmetic() {
    TextBuf prog;
    repeat(prog, "INC", 64);
    {
        Fix f; // 64 codons: (64/64)^(e-1) = 1, so u * 64 = 128 a tick, any exponent
        TextBuf cfg;
        cfg.append(kCalm);
        cfg.append("world.millicores = 4000\nmem.upkeep = 2\n");
        CHECK(f.init(cfg.c_str()));
        u32 b = plant(f.s, 100, prog.c_str(), 1000);
        CHECK(b != kNone);
        f.s.tick(0);
        CHECK(f.s.cpu(f.s.body(b).cpus).cx == 1000 - 128);
        CHECK(f.s.tally().upkeep == 128);
        CHECK(f.s.tally().instructions == 872);
        CHECK(f.s.body(b).work == 1000); // upkeep + instructions: the heat input
    }
    {
        Fix f; // linear: u * m exactly, independent of size
        TextBuf cfg;
        cfg.append(kCalm);
        cfg.append("world.millicores = 4000\nmem.upkeep = 0.5\nmem.upkeep_exp = 1\n");
        CHECK(f.init(cfg.c_str()));
        u32 b = plant(f.s, 100, "INC INC INC INC INC INC INC INC", 1000);
        f.s.tick(0);
        CHECK(f.s.cpu(f.s.body(b).cpus).cx == 996); // 1000 - 0.5 * 8
    }
    {
        Fix f; // square-cube: 512 codons cost u * 512 * 8^(1/3) = 2048 a tick
        TextBuf cfg, big;
        cfg.append(kCalm);
        cfg.append("world.millicores = 4000\nmem.upkeep = 2\n");
        CHECK(f.init(cfg.c_str()));
        repeat(big, "INC", 512);
        u32 b = plant(f.s, 100, big.c_str(), 4000);
        CHECK(b != kNone);
        f.s.tick(0);
        CHECK(f.s.tally().upkeep >= 2047 && f.s.tally().upkeep <= 2048);
    }
}

// What the budget cannot cover is carried as debt; with heat off the body
// just stops running (heat is what turns debt into death).
void upkeep_debt() {
    Fix f;
    TextBuf cfg, prog;
    cfg.append(kCalm);
    cfg.append("world.millicores = 4000\nmem.upkeep = 2\n");
    CHECK(f.init(cfg.c_str()));
    repeat(prog, "INC", 64);
    u32 b = plant(f.s, 100, prog.c_str(), 10); // earns 10 a tick, owes 128
    for (u64 t = 0; t < 3; ++t)
        f.s.tick(t);
    CHECK(f.s.cpu(f.s.body(b).cpus).cx == 0);
    CHECK(fabs(f.s.body(b).carry - (3 * (10.0 - 128.0))) < 1e-3);
    CHECK(f.s.body_count() == 1);
    CHECK(soup_ok(f.s));
}

// With heat on, an over-extended body throttles, loses its millicores and dies,
// and its memory comes back -- the "giant claims self-destruct" case of D1.
void upkeep_kills() {
    {
        Fix f;
        CHECK(f.init("hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nworld.millicores = 4000\n"));
        TextBuf prog;
        repeat(prog, "INC", 64);
        u32 b = plant(f.s, 100, prog.c_str(), 10);
        CHECK(b != kNone);
        u64 t = 0;
        while (f.s.body_count() > 0 && t < 10)
            f.s.tick(t++);
        CHECK(f.s.body_count() == 0 && t <= 3);
        CHECK(f.s.waste() == 4096 && f.s.free_units() == 4000);
    }
    {
        Fix f; // a 10-millicore body holding 30,000 codons owes ~466k a tick
        CHECK(f.init("hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nworld.millicores = 4000\n",
                     65536));
        u32 b = f.s.create_body(1000, 30000, 10);
        CHECK(b != kNone);
        f.s.tick(0);
        CHECK(f.s.tally().upkeep > 460000 && f.s.tally().upkeep < 470000);
        CHECK(f.s.body_count() == 0 && f.s.waste() == 65536);
    }
    {
        Fix f; // a lean body pays its way: an 8-codon replicator mid-copy pays ~8%
        CHECK(f.init("hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nworld.millicores = 4000\n"
                     "cpu.instr_per_core = 25000\n"));
        u32 b = plant(f.s, 100, "SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE", 10);
        for (u64 t = 0; t < 50; ++t)
            f.s.tick(t);
        CHECK(f.s.body(b).alive);
    }
}

// With the Phase 1b rule changes switched off (kPhase1aRules) the engine is
// exactly Phase 1a: the busy soup world hashes to the value Phase 1a's engine
// gave (recorded from the unchanged Phase 1a build). The default rules move it,
// to a value pinned here.
void pinned_hashes() {
    TextBuf cfg;
    cfg.append(kSoupWorld);
    cfg.append(kPhase1aRules);
    Run phase1a;
    CHECK(make_run(phase1a, cfg.c_str(), 4));
    phase1a.run_until(300);
    CHECK_EQ_U64(phase1a.world().hash(), 0xdecce3c361397145ull);

    Run now;
    CHECK(make_run(now, kSoupWorld, 4));
    now.run_until(300);
    printf("  rules: busy soup world at tick 300: %016llx (Phase 1b defaults), "
           "decce3c361397145 (Phase 1a rules)\n",
           static_cast<unsigned long long>(now.world().hash()));
    CHECK_EQ_U64(now.world().hash(), kPinnedDefault);
}

// CPU wear returns held millicores to the pool at the configured rate.
void cpu_wear() {
    Fix f;
    TextBuf cfg;
    cfg.append(kCalm);
    cfg.append("world.millicores = 4000\nmem.upkeep = 0\ncpu.wear = 0.01\n");
    CHECK(f.init(cfg.c_str()));
    u32 b = plant(f.s, 100, "INC", 4000);
    f.s.tick(0);
    u32 worn = static_cast<u32>(f.s.tally().cpu_worn);
    CHECK(worn > 20 && worn < 70); // ~40 expected (4000 x 0.01)
    CHECK(f.s.body(b).units == 4000 - worn && f.s.free_units() == worn);
    CHECK(soup_ok(f.s));

    Fix g; // off by default
    TextBuf off;
    off.append(kCalm);
    off.append("world.millicores = 4000\nmem.upkeep = 0\n");
    CHECK(g.init(off.c_str()));
    u32 c = plant(g.s, 100, "INC", 4000);
    for (u64 t = 0; t < 20; ++t)
        g.s.tick(t);
    CHECK(g.s.body(c).units == 4000 && g.s.tally().cpu_worn == 0);
}

// spark.timing = last: sparks take only what the bodies left. A body that
// claims every freed millicore each tick then starves the sparks completely;
// with sparks first they get the worn units before it can.
void spark_timing() {
    u64 sparks[2] = {};
    for (int late = 0; late < 2; ++late) {
        Fix f;
        TextBuf cfg, prog;
        cfg.append(kCalm);
        cfg.append("world.millicores = 1000\nmem.upkeep = 0\ncpu.wear = 0.05\nspark.rate = 2\n"
                   "spark.grant = 10\nspark.min_len = 8\nspark.max_len = 8\n");
        cfg.append(late ? "spark.timing = last\n" : "spark.timing = first\n");
        CHECK(f.init(cfg.c_str()));
        // SENSE 1 (memory I own = 128) then ALLOC 1 (claim that many millicores).
        prog.append("ZERO INC ROTB SENSE ALLOC ");
        repeat(prog, "NOP0", 123);
        CHECK(plant(f.s, 100, prog.c_str(), 1000) != kNone);
        for (u64 t = 0; t < 30; ++t) {
            f.s.tick(t);
            sparks[late] += f.s.tally().sparks;
        }
        CHECK(soup_ok(f.s));
    }
    CHECK(sparks[0] > 10);
    CHECK(sparks[1] == 0);
}

// Spark grant and lengths are live knobs (D4), logged and replayed like
// spark.rate; a min above the max is read as the range between them.
void spark_knobs() {
    const char* world = "seed = 5\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 8192\n"
                        "world.millicores = 4000\nheat.enabled = false\nhazard.copy = 0\n"
                        "hazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\nspark.timing = last\n"
                        "spark.rate = 3\nspark.grant = 10\nspark.min_len = 8\nspark.max_len = 64\n";
    Run run;
    CHECK(make_run(run, world, 1));
    TextBuf err;
    CHECK(run.schedule_knob(5, "spark.grant", "3", &err));
    CHECK(run.schedule_knob(5, "spark.min_len", "20", &err));
    CHECK(run.schedule_knob(5, "spark.max_len", "20", &err));
    CHECK(run.schedule_knob(8, "spark.min_len", "30", &err)); // above the max of 20
    run.run_until(5);
    // Bodies sparked at tick 5. Sparks fire last, so they are the newest bodies
    // and have not run yet.
    run.step();
    const Soup& s = run.world().node_soup(0);
    bool shaped = run.world().last_soup_tick().sparks > 0;
    u64 newest = 0;
    for (usize k = 0; k < s.body_count(); ++k)
        newest = max_of<u64>(newest, s.body(s.body_at(k)).serial);
    u32 fresh = 0;
    for (usize k = 0; k < s.body_count(); ++k) {
        const Body& b = s.body(s.body_at(k));
        if (b.serial + run.world().last_soup_tick().sparks > newest) {
            ++fresh;
            shaped = shaped && s.block(b.blocks).len == 20 && b.units == 3;
        }
    }
    CHECK(shaped && fresh == run.world().last_soup_tick().sparks);
    CHECK(run.config().get_str("spark.grant", "").eq("3"));

    run.run_until(9); // tick 8: min 30, max 20 -> lengths 20..30
    const Soup& s2 = run.world().node_soup(0);
    u64 n8 = run.world().last_soup_tick().sparks;
    newest = 0;
    for (usize k = 0; k < s2.body_count(); ++k)
        newest = max_of<u64>(newest, s2.body(s2.body_at(k)).serial);
    bool ranged = n8 > 0;
    for (usize k = 0; k < s2.body_count(); ++k) {
        const Body& b = s2.body(s2.body_at(k));
        if (b.serial + n8 > newest) {
            u32 len = s2.block(b.blocks).len;
            ranged = ranged && len >= 20 && len <= 30;
        }
    }
    CHECK(ranged);

    // Out-of-range values and the new structural keys are refused. (cpu.wear
    // became a live knob in Phase 6, design.md 6.12; out of range it's refused.)
    CHECK(!run.schedule_knob(20, "spark.grant", "0", &err));
    CHECK(!run.schedule_knob(20, "spark.max_len", "9000", &err));
    CHECK(!run.schedule_knob(20, "mem.upkeep", "1", &err));
    CHECK(!run.schedule_knob(20, "cpu.wear", "1.5", &err));
    CHECK(!run.schedule_knob(20, "cpu.instr_per_core", "100", &err));
    CHECK(!run.schedule_knob(20, "detect.similarity", "0.5", &err));
}

// INJECT's criteria (design.md 6.6, Damian): it needs a key (the template
// after it) complementing the host's surface -- blind injectors and surfaceless
// hosts get nowhere -- and a host big enough to fit the payload, or it fails
// leaving the host untouched. Each can be switched back to Phase 1a.
struct Injection {
    bool ok;     // the INJECT succeeded
    u32 cost;    // instructions charged
    bool intact; // the host's code is unchanged
};

Injection inject_once(const char* injector, const char* host, u32 cx, const char* extra = "") {
    Fix f;
    TextBuf cfg;
    cfg.append(kCalm);
    cfg.append("world.millicores = 1000\nmem.upkeep = 0\n");
    cfg.append(extra);
    Injection r = {};
    if (!f.init(cfg.c_str(), 1024))
        return r;
    u32 a = plant(f.s, 100, injector, 500);
    u32 h = plant(f.s, 200, host, 100);
    if (a == kNone || h == kNone)
        return r;
    u8 before[16];
    const u32 len = f.s.block(f.s.body(h).blocks).len;
    for (u32 i = 0; i < len && i < 16; ++i)
        before[i] = f.s.mem()[200 + i];
    f.s.begin_tick(0);
    Cpu& c = f.s.cpu_mut(f.s.body(a).cpus);
    c.cx = cx;
    c.rh = 100;
    r.cost = static_cast<u32>(f.s.run_cpu(f.s.body(a).cpus, 1));
    r.ok = f.s.tally().injects == 1;
    r.intact = true;
    for (u32 i = 0; i < len && i < 16; ++i)
        r.intact = r.intact && f.s.mem()[200 + i] == before[i];
    return r;
}

void inject_rules() {
    // A key needs a surface it complements: "0" against "1" lands for sure.
    Injection keyed = inject_once("INJECT NOP0 INC INC", "NOP1 INC INC INC", 2);
    CHECK(keyed.ok && !keyed.intact && keyed.cost == 9 + 2);
    // Blind injectors and surfaceless hosts get nowhere (they did in Phase 1a).
    CHECK(!inject_once("INJECT INC INC INC", "INC INC INC INC", 2).ok);
    CHECK(inject_once("INJECT INC INC INC", "INC INC INC INC", 2, "contact.inject_key = false\n").ok);
    CHECK(!inject_once("INJECT NOP0 INC INC", "INC INC INC INC", 2).ok);
    CHECK(!inject_once("INJECT INC INC INC", "NOP1 INC INC INC", 2).ok);
    // Strengths and weaknesses: the key "00" opens a "11" surface and not a "00" one.
    CHECK(inject_once("INJECT NOP0 NOP0 INC", "NOP1 NOP1 INC INC", 1).ok);
    CHECK(!inject_once("INJECT NOP0 NOP0 INC", "NOP0 NOP0 INC INC", 1).ok);
    // The payload must fit: 5 codons into a 4-codon host fails, leaving it
    // intact, and still costs the attempt; 4 fit.
    Injection big = inject_once("INJECT NOP0 INC INC INC", "NOP1 INC INC INC", 5);
    CHECK(!big.ok && big.intact && big.cost == 9);
    CHECK(inject_once("INJECT NOP0 INC INC INC", "NOP1 INC INC INC", 4).ok);
    // contact.inject_room = 0: Phase 1a truncates the payload to the host instead.
    CHECK(inject_once("INJECT NOP0 INC INC INC", "NOP1 INC INC INC", 5, "contact.inject_room = 0\n").ok);
    // room 2: the host must be twice the payload.
    CHECK(!inject_once("INJECT NOP0 INC INC INC", "NOP1 INC INC INC", 3, "contact.inject_room = 2\n").ok);
    CHECK(inject_once("INJECT NOP0 INC INC INC", "NOP1 INC INC INC", 2, "contact.inject_room = 2\n").ok);
}

// Bad values for the new structural keys fail loudly.
void config_errors() {
    SoupParams p;
    CHECK(!make_params(&p, "mem.upkeep = -1\n", 1024));
    CHECK(!make_params(&p, "mem.upkeep_exp = 9\n", 1024));
    CHECK(!make_params(&p, "cpu.wear = 2\n", 1024));
    CHECK(!make_params(&p, "spark.timing = never\n", 1024));
    CHECK(!make_params(&p, "contact.inject_room = -1\n", 1024));
    CHECK(make_params(&p, "", 1024) && p.inject_key && p.inject_room == 1.0);
    CHECK(make_params(&p, "spark.timing = last\ncpu.wear = 0.001\n", 1024));
    CHECK(p.spark_late && p.cpu_wear == 0.001 && p.upkeep == 2.0 && p.upkeep_exp == 4.0 / 3.0);
}

} // namespace

void suite_rules() {
    upkeep_arithmetic();
    upkeep_debt();
    upkeep_kills();
    pinned_hashes();
    cpu_wear();
    spark_timing();
    spark_knobs();
    inject_rules();
    config_errors();
}
