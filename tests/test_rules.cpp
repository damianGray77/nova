// Phase 1b rules (design.md 6.6): memory upkeep (D1), CPU wear and spark
// timing (D10), live spark knobs (D4), and the pinned hashes -- Phase 1a's at
// mem.upkeep = 0 and the new default's.
#include <cmath>

#include "check.h"
#include "helpers.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// The busy soup world (helpers.h) at tick 300 under the defaults, and with
// Phase 1a's rules (kPhase1aRules). Re-pinned 2026-11-08 for docs/bites-plan.md
// (INJECT only through wounds, bleeding, bites without immunity); the second
// again 2026-11-09 for docs/inject-plan.md (INJECT moves the program).
constexpr u64 kPinnedDefault = 0xf537de099f13cb24ull;
constexpr u64 kPinnedPhase1aRules = 0xff50968554057dcbull;

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

// The busy soup world at tick 300, pinned with the default rules and with the
// Phase 1b rule changes switched off (kPhase1aRules). Until 2026-11-08 the
// second was exactly Phase 1a (decce3c361397145, from the unchanged Phase 1a
// build); since INJECT lands only through a wound and open ends bleed
// (docs/bites-plan.md) Phase 1a can't be reproduced, and the pin guards that
// world's determinism instead.
void pinned_hashes() {
    TextBuf cfg;
    cfg.append(kSoupWorld);
    cfg.append(kPhase1aRules);
    Run phase1a;
    CHECK(make_run(phase1a, cfg.c_str(), 4));
    phase1a.run_until(300);

    Run now;
    CHECK(make_run(now, kSoupWorld, 4));
    now.run_until(300);
    printf("  rules: busy soup world at tick 300: %016llx (defaults), %016llx (Phase 1a's rules)\n",
           static_cast<unsigned long long>(now.world().hash()), static_cast<unsigned long long>(phase1a.world().hash()));
    CHECK_EQ_U64(now.world().hash(), kPinnedDefault);
    CHECK_EQ_U64(phase1a.world().hash(), kPinnedPhase1aRules);
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

// INJECT's criteria (docs/inject-plan.md M1, M2, M6, M7; docs/bites-plan.md
// I1): it lands only through an open end of the host's code (an end whose
// outermost codon isn't a NOP: a wound), only on a host owning at least
// contact.inject_ratio (2) x the injector's memory, whose code has room for
// the injector's genome. The whole injector moves in: its genome overwrites
// the host's codons just inside the wound, its thread carries on there as a
// parasite, and the memory it leaves is zeros. A failure leaves both as they
// were and costs the attempt.
struct Injection {
    bool ok;          // the INJECT succeeded
    u32 cost;         // instructions charged
    bool intact;      // the host's code is unchanged
    u8 before[16];    // the host's code before
    u8 after[16];     // the host's code afterwards
    u8 inj[16];       // the injector's code before
    bool zeroed;      // the injector's old memory is zeros and nobody's
    bool parasite;    // the injector is its host's parasite now
    u32 ip;           // the injector's thread's IP afterwards
    u32 host_threads; // the host's own threads afterwards
    bool sound;       // Soup::check passes
};

Injection inject_once(const char* injector, const char* host, const char* extra = "") {
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
    const u32 len = f.s.block(f.s.body(h).blocks).len, ilen = f.s.block(f.s.body(a).blocks).len;
    for (u32 i = 0; i < len && i < 16; ++i)
        r.before[i] = f.s.mem()[200 + i];
    for (u32 i = 0; i < ilen && i < 16; ++i)
        r.inj[i] = f.s.mem()[100 + i];
    f.s.begin_tick(0);
    const u32 ca = f.s.body(a).cpus;
    r.cost = static_cast<u32>(f.s.run_cpu(ca, 1));
    r.ok = f.s.tally().injects == 1;
    r.intact = true;
    for (u32 i = 0; i < len && i < 16; ++i) {
        r.after[i] = f.s.mem()[200 + i];
        r.intact = r.intact && f.s.mem()[200 + i] == r.before[i];
    }
    r.zeroed = true;
    for (u32 i = 0; i < ilen; ++i)
        r.zeroed = r.zeroed && f.s.mem()[100 + i] == 0 && f.s.owner_body(100 + i) == kNone;
    r.parasite = f.s.is_parasite(a) && f.s.host_of(a) == h && f.s.body(a).blocks == kNone && f.s.body(a).alive;
    r.ip = f.s.cpu(ca).ip;
    r.host_threads = f.s.body(h).ncpu;
    r.sound = soup_ok(f.s);
    return r;
}

void inject_rules() {
    // A tail wound ("NOP1 INC ... INC": its high end isn't a NOP): the
    // injector's 3-codon genome lands just inside it, the host's last three
    // codons; its thread runs on there (at INC, past INJECT's template), the
    // host's own thread is the only one the host has, and the injector's old
    // memory is zeros. The price: the attempt (1 + contact.cost_base 8) and
    // contact.cost_per_codon (1) per codon moved (M7).
    Injection tail = inject_once("INJECT NOP0 INC", "NOP1 INC INC INC INC INC");
    CHECK(tail.ok && !tail.intact && tail.cost == 9 + 3 && tail.sound);
    CHECK(tail.after[0] == tail.before[0] && tail.after[2] == tail.before[2]);
    CHECK(tail.after[3] == tail.inj[0] && tail.after[4] == tail.inj[1] && tail.after[5] == tail.inj[2]);
    CHECK(tail.zeroed && tail.parasite && tail.ip == 205 && tail.host_threads == 1);
    // A head wound: its first three codons.
    Injection head = inject_once("INJECT NOP0 INC", "INC INC INC INC INC NOP1");
    CHECK(head.ok && head.after[0] == head.inj[0] && head.after[1] == head.inj[1] && head.after[2] == head.inj[2]);
    CHECK(head.after[5] == head.before[5] && head.ip == 202 && head.parasite && head.sound);
    // Blind or aimed, a host with an open end is open; one sealed by NOPs at
    // both ends is closed.
    CHECK(inject_once("INJECT INC INC", "INC INC INC INC INC INC").ok);
    CHECK(!inject_once("INJECT INC INC", "NOP1 INC INC INC INC NOP0").ok);
    // M6 (Damian): the host must own twice the injector's memory. 5 codons
    // against 3 fails, leaving both as they were, and costs the attempt.
    Injection small = inject_once("INJECT NOP0 INC", "NOP1 INC INC INC INC");
    CHECK(!small.ok && small.intact && small.cost == 9 && !small.parasite && small.ip == 102 && small.sound);
    CHECK(inject_once("INJECT NOP0 INC", "NOP1 INC INC INC INC", "contact.inject_ratio = 1.5\n").ok);
    // An equal-sized host (a bud) never qualifies, nor a smaller one.
    CHECK(!inject_once("INJECT NOP0 INC", "NOP1 INC INC").ok);
    // With the ratio off, the room rule: the host's code must hold the genome,
    // 3 codons into 2 fails; contact.inject_room = 0 truncates it to the host.
    CHECK(!inject_once("INJECT NOP0 INC", "NOP1 INC", "contact.inject_ratio = 0\n").ok);
    Injection cut = inject_once("INJECT NOP0 INC", "NOP1 INC", "contact.inject_ratio = 0\ncontact.inject_room = 0\n");
    CHECK(cut.ok && cut.after[0] == cut.inj[0] && cut.after[1] == cut.inj[1] && cut.zeroed && cut.sound);
    // room 2: the host's code must be twice the genome.
    CHECK(!inject_once("INJECT NOP0 INC", "NOP1 INC INC INC INC", "contact.inject_ratio = 0\ncontact.inject_room = 2\n").ok);
    CHECK(inject_once("INJECT NOP0 INC", "NOP1 INC INC INC INC INC", "contact.inject_ratio = 0\ncontact.inject_room = 2\n").ok);
}

// Bad values for the new structural keys fail loudly.
void config_errors() {
    SoupParams p;
    CHECK(!make_params(&p, "mem.upkeep = -1\n", 1024));
    CHECK(!make_params(&p, "mem.upkeep_exp = 9\n", 1024));
    CHECK(!make_params(&p, "cpu.wear = 2\n", 1024));
    CHECK(!make_params(&p, "spark.timing = never\n", 1024));
    CHECK(!make_params(&p, "contact.inject_room = -1\n", 1024));
    CHECK(make_params(&p, "", 1024) && p.inject_room == 1.0 && p.bleed_every == 100 && p.tissue);
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
