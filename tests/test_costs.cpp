// Measured costs (Phase 9, design.md 6.15, docs/phase9-plan.md): the cost
// tables, world time charged instruction by instruction, the CPU's time a
// tick, and actions in progress -- ENGULF and INJECT with a start, a wait and
// an end, a target that defends itself by changing in time, an actor lost
// mid-action, a DISK op longer than a tick -- plus the determinism gates and
// the `defended` milestone. Phase 9b and 9c: the devices' clocks; threads
// (a core each, slices) and E cores (their costs, time and heat, and their
// millicores through ALLOC, DIVIDE, ENGULF, cargo and landscapes).
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

struct Fix {
    SoupParams p;
    Soup s;
    bool init(const char* extra = "", u32 size = 1024) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\ncost.table = home_v1\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
    u32 cpu_of(u32 b) const { return s.body(b).cpus; }
    Cpu& cpu(u32 b) { return s.cpu_mut(s.body(b).cpus); }
};

// The two built-in tables, and the recipes' own parts.
void test_tables() {
    CostTable c, h, x;
    CHECK(builtin_costs("classic", &c) && !c.timed);
    bool ones = true;
    for (u32 i = 0; i < kOpCount; ++i)
        ones = ones && c.op[i] == 1;
    CHECK(ones);
    CHECK(builtin_costs("home_v1", &h) && h.timed == 1);
    CHECK(h.alu == 190 && h.div == 2650 && h.call == 389000);
    CHECK(h.op[static_cast<u32>(Op::inc)] == h.alu);
    CHECK(h.op[static_cast<u32>(Op::div)] == h.div && h.op[static_cast<u32>(Op::mul)] == h.mul);
    CHECK(h.op[static_cast<u32>(Op::copy)] == h.load + h.store);
    CHECK(h.op[static_cast<u32>(Op::emit)] == h.share && h.op[static_cast<u32>(Op::rand)] == h.draw);
    CHECK(!builtin_costs("home_v9", &x));
    TextBuf d;
    describe_costs(h, d);
    CHECK(d.size() > 1000 && strstr(d.c_str(), "home_v1") != nullptr);

    // A bad name is a config error.
    SoupParams p;
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse("cost.table = fast\n", &pe));
    CHECK(!soup_params_from_config(cfg, 1024, &p));
}

// Each instruction is charged its own part plus what it does as it runs.
void test_charges() {
    Fix f;
    CHECK(f.init());
    const CostTable& t = f.p.cost;
    const u32 b = plant(f.s, 100, "INC MUL DIV ADD JMP NOP0 INC NOP1", 1000);
    CHECK(b != kNone);
    const u32 c = f.cpu_of(b);
    CHECK(f.s.run_cpu(c, 1) == t.alu);  // INC
    CHECK(f.s.run_cpu(c, 1) == t.mul);  // MUL
    CHECK(f.s.run_cpu(c, 1) == t.div);  // DIV (by 0: it fails, and still costs a divide)
    CHECK(f.s.cpu(c).err == 1);
    CHECK(f.s.run_cpu(c, 1) == t.alu);  // ADD
    // JMP NOP0: its one template codon read, three places searched (the INC
    // after it, the ADD before it, then the NOP1), and the jump taken.
    CHECK(f.s.run_cpu(c, 1) == t.alu + t.load + 3 * t.scan + t.jump);
    CHECK(f.s.cpu(c).ip == 100); // after the NOP1: its block's end, so its start
    CHECK(soup_ok(f.s));
}

// A core's time a tick is cpu.instr_per_core adds: so 100 INCs, but only 33
// MULs (578 ps each: the 33rd runs over and is carried as debt).
void test_budget() {
    for (int k = 0; k < 2; ++k) {
        Fix f;
        CHECK(f.init("cpu.instr_per_core = 100\n"));
        const u32 b = plant(f.s, 100, k == 0 ? "INC INC INC INC" : "MUL MUL MUL MUL", 1000);
        f.s.tick(0);
        const u64 ran = f.s.cpu(f.cpu_of(b)).executed;
        CHECK(ran == (k == 0 ? 100u : 33u));
        CHECK(f.p.tick_units == 100.0 * f.p.cost.alu);
    }
    // classic: 100 of either
    SoupParams p;
    CHECK(make_params(&p, "heat.enabled = false\nmem.upkeep = 0\nworld.millicores = 2000\ncpu.instr_per_core = 100\n",
                      1024));
    Soup s;
    s.init(&p, 23, 0);
    s.begin_tick(0);
    const u32 b = plant(s, 100, "MUL MUL MUL MUL", 1000);
    s.tick(0);
    CHECK(s.cpu(s.body(b).cpus).executed == 100);
}

// ENGULF as an action: started (the prey marked, SENSE 22 and 23 see it),
// the eater's CPU waits, and at its end the prey is eaten.
void test_engulf_action() {
    Fix f;
    CHECK(f.init());
    const u32 eater = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    const u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 SENSE", 300); // surface 110: a perfect match
    CHECK(eater != kNone && prey != kNone);
    const i64 used = f.s.run_cpu(f.cpu_of(eater), 1000000);
    CHECK(used == 1000000); // the rest of the tick goes on the wait
    CHECK(f.s.actions().size() == 1 && f.s.tally().actions_started == 1 && f.s.tally().engulfs == 0);
    CHECK(f.cpu(eater).ip == 100 && f.s.body(prey).alive);
    const Action& a = f.s.actions()[0];
    CHECK(a.target == prey && a.end == 1 && a.op == static_cast<u32>(Op::engulf)); // 4 codons: one tick
    // The prey senses it.
    Cpu& pc = f.cpu(prey);
    pc.ip = 203;
    pc.bx = 22;
    f.s.run_cpu(f.cpu_of(prey), 1);
    CHECK(f.cpu(prey).cx == 1); // one tick to go
    pc.ip = 203;
    pc.bx = 23;
    f.s.run_cpu(f.cpu_of(prey), 1);
    CHECK(f.cpu(prey).cx == kActEngulf);
    // Still tick 0: the eater waits.
    const u64 before = f.cpu(eater).executed;
    f.s.run_cpu(f.cpu_of(eater), 1000000);
    CHECK(f.cpu(eater).executed == before && f.s.actions().size() == 1);
    CHECK(soup_ok(f.s));
    // Tick 1: it lands (a quota of 0: just the landing, nothing after).
    f.s.begin_tick(1);
    f.s.run_cpu(f.cpu_of(eater), 0);
    CHECK(f.s.tally().engulfs == 1 && f.s.tally().actions_landed == 1 && f.s.actions().empty());
    CHECK(!f.s.body(prey).alive && f.s.owner_body(200) == eater && f.cpu(eater).ip == 104);
    CHECK(f.cpu(eater).err == 0);

    // A bigger prey takes longer: 40 codons at 16 a tick is 3 ticks.
    Fix g;
    CHECK(g.init());
    const u32 e2 = plant(g.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    TextBuf big;
    big.append("NOP1 NOP1 NOP0");
    for (int i = 0; i < 37; ++i)
        big.append(" INC");
    CHECK(plant(g.s, 200, big.c_str(), 300) != kNone);
    g.s.run_cpu(g.cpu_of(e2), 1000);
    CHECK(g.s.actions().size() == 1 && g.s.actions()[0].end == 3);
}

// A target that changes in time: the eater's aim no longer matches at the end,
// so the ENGULF fails and counts as a defence.
void test_defence() {
    Fix f;
    CHECK(f.init());
    const u32 eater = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    const u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 INC", 300);
    f.s.run_cpu(f.cpu_of(eater), 1000);
    CHECK(f.s.actions().size() == 1);
    // The prey rewrites its surface (as a WRITE at its own code would) to
    // match nothing of the aim: 001 against the template's 001.
    const u8 nop0 = static_cast<u8>(codon_of(f.p.table, Op::nop0));
    const u8 nop1 = static_cast<u8>(codon_of(f.p.table, Op::nop1));
    f.s.mem_mut()[200] = nop0;
    f.s.mem_mut()[201] = nop0;
    f.s.mem_mut()[202] = nop1;
    f.s.begin_tick(1);
    f.s.run_cpu(f.cpu_of(eater), 0);
    CHECK(f.s.tally().actions_failed == 1 && f.s.tally().actions_defended == 1 && f.s.tally().engulfs == 0);
    CHECK(f.s.body(prey).alive && f.cpu(eater).err == 1 && f.s.actions().empty());
    CHECK(f.cpu(eater).ip == 104);

    // Unchanged, the same attack lands (the chance is 3/3).
    Fix g;
    CHECK(g.init());
    const u32 e2 = plant(g.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    const u32 p2 = plant(g.s, 200, "NOP1 NOP1 NOP0 INC", 300);
    g.s.run_cpu(g.cpu_of(e2), 1000);
    g.s.begin_tick(1);
    g.s.run_cpu(g.cpu_of(e2), 0);
    CHECK(!g.s.body(p2).alive && g.s.tally().actions_defended == 0);
}

// An actor that dies mid-action loses it; its target is no longer marked.
// And a target eaten by someone else ends the action at once, failed.
void test_actor_lost() {
    Fix f;
    CHECK(f.init());
    const u32 eater = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    const u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 SENSE", 300);
    f.s.run_cpu(f.cpu_of(eater), 1000);
    CHECK(f.s.action_on(prey) != kNone);
    f.cpu(eater).halted = 1; // it dies in the sweep
    f.s.tick_second();
    CHECK(!f.s.body(eater).alive && f.s.actions().empty() && f.s.tally().actions_failed == 1);
    CHECK(f.s.action_on(prey) == kNone);
    CHECK(soup_ok(f.s));

    Fix g;
    CHECK(g.init());
    const u32 e1 = plant(g.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    TextBuf code;
    code.append("NOP1 NOP1 NOP0");
    for (int i = 0; i < 33; ++i)
        code.append(" INC");
    const u32 p1 = plant(g.s, 200, code.c_str(), 300); // 36 codons: three ticks
    g.s.run_cpu(g.cpu_of(e1), 1000);
    CHECK(g.s.actions().size() == 1 && g.s.actions()[0].end == 3);
    g.cpu(p1).halted = 1; // the prey dies first
    g.s.tick_second();
    g.s.begin_tick(1);
    g.s.run_cpu(g.cpu_of(e1), 0);
    CHECK(g.s.actions().empty() && g.s.tally().actions_failed == 1 && g.cpu(e1).err == 1);
}

// INJECT as an action, priced like a bite (docs/inject-plan.md M7): it lasts
// the codons it moves at engulf.codons_per_tick (16 in the suites, 8 by
// default), its thread busy for the
// rest of each tick meanwhile, and the program moves in at the end, just
// inside the host's open end (its tail: 205-209).
void test_inject_action() {
    Fix f;
    CHECK(f.init());
    const u32 inj = plant(f.s, 100, "INJECT NOP0 NOP0 NOP1 INC", 500); // 5 codons: one tick
    const u32 host = plant(f.s, 200, "NOP1 NOP1 NOP0 INC INC INC INC INC INC INC", 300); // 10: twice its size
    CHECK(inj != kNone && host != kNone);
    u8 genome[5];
    for (u32 i = 0; i < 5; ++i)
        genome[i] = f.s.mem()[100 + i];
    const i64 used = f.s.run_cpu(f.cpu_of(inj), 1000000);
    CHECK(used == 1000000 && f.s.actions().size() == 1 && f.s.actions()[0].end == 1); // busy: the CPU at work
    f.s.begin_tick(1);
    f.s.run_cpu(f.cpu_of(inj), 0);
    CHECK(f.s.tally().injects == 1 && f.s.tally().actions_landed == 1 && f.s.is_parasite(inj));
    for (u32 i = 0; i < 5; ++i)
        CHECK(f.s.mem()[205 + i] == genome[i] && f.s.mem()[100 + i] == 0);
    CHECK(soup_ok(f.s));
    // A bigger program takes longer: 20 codons, two ticks at the suites' 16
    // (three at the default 8).
    Fix h;
    CHECK(h.init());
    TextBuf big, bigger;
    big.append("INJECT");
    for (int i = 0; i < 19; ++i)
        big.append(" INC");
    bigger.append("NOP1");
    for (int i = 0; i < 39; ++i)
        bigger.append(" INC");
    const u32 i3 = plant(h.s, 100, big.c_str(), 500);
    CHECK(i3 != kNone && plant(h.s, 200, bigger.c_str(), 300) != kNone);
    h.s.run_cpu(h.cpu_of(i3), 1000000);
    CHECK(h.s.actions().size() == 1 && h.s.actions()[0].end == (20 + h.p.engulf_cpt - 1) / h.p.engulf_cpt);
    CHECK(h.p.engulf_cpt == 16 && h.s.actions()[0].end == 2);
    // A host that seals its wound meanwhile defends itself.
    Fix g;
    CHECK(g.init());
    const u32 i2 = plant(g.s, 100, "INJECT NOP0 NOP0 NOP1 INC", 500);
    const u32 h2 = plant(g.s, 200, "NOP1 NOP1 NOP0 INC INC INC INC INC INC INC", 300);
    CHECK(i2 != kNone && h2 != kNone);
    g.s.run_cpu(g.cpu_of(i2), 1000);
    CHECK(g.s.actions().size() == 1);
    g.s.mem_mut()[209] = static_cast<u8>(codon_of(g.p.table, Op::nop0)); // the host writes a NOP over its tail
    g.s.begin_tick(1);
    g.s.run_cpu(g.cpu_of(i2), 0);
    CHECK(g.s.tally().injects == 0 && g.s.tally().actions_defended == 1 && !g.s.is_parasite(i2));
}

// A DISK store longer than a tick (a 12 us write against a 4.75 us tick):
// the CPU waits three ticks, then the codons are written.
void test_disk_action() {
    Fix f;
    CHECK(f.init("world.disk = 4096\ncpu.instr_per_core = 25000\n"));
    const u32 b = plant(f.s, 100, "ALLOC DISK INC INC INC INC INC INC INC INC INC INC INC INC INC INC INC", 1000);
    CHECK(b != kNone);
    Cpu& c = f.cpu(b);
    c.bx = 3;  // ALLOC: disk
    c.cx = 16;
    f.s.run_cpu(f.cpu_of(b), 1);
    CHECK(f.s.disk_held(b) == 16);
    c.bx = 1; // DISK: store
    c.ax = 0;
    c.cx = 16;
    c.rh = 100;
    f.s.run_cpu(f.cpu_of(b), 100000000);
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].op == static_cast<u32>(Op::disk));
    CHECK(f.s.actions()[0].end == 3 && f.s.tally().disk_stores == 0);
    for (u64 t = 1; t < 3; ++t) {
        f.s.begin_tick(t);
        f.s.run_cpu(f.cpu_of(b), 100000000);
        CHECK(f.s.actions().size() == 1 && f.s.tally().disk_stores == 0);
    }
    f.s.begin_tick(3);
    f.s.run_cpu(f.cpu_of(b), 0);
    CHECK(f.s.tally().disk_stores == 1 && f.s.tally().actions_landed == 1 && f.s.actions().empty());
    CHECK(soup_ok(f.s));
}

// A world with home_v1: kSoupWorld1b's busy world, with a core's tick at
// 25,000 adds (4.75 us) so that a kernel call (389 ns) is a fraction of a
// tick, and its seeded replicator funded to match. Deterministic at 1, 4 and
// 20 threads and under shuffled dispatch, exact through a snapshot taken with
// actions in progress, and another world than classic. classic named
// outright hashes as no key.
const char* const kTimedWorld = "seed = 42\n"
                                "world.model = soup\n"
                                "world.nodes = 24\n"
                                "world.chords = 12\n"
                                "world.soup_size = 2048\n"
                                "world.millicores = 400\n"
                                "cpu.instr_per_core = 25000\n"
                                "cpu.wear = 0.002\n"
                                "isa.table = nova_v1_inv2\n"
                                "spark.timing = last\n"
                                "spark.initial = 20\n"
                                "spark.rate = 0.5\n"
                                "spark.grant = 20\n"
                                "spark.min_len = 8\n"
                                "spark.max_len = 24\n"
                                // (a COPY replicator sealed by NOPs, as kSoupWorld1b's: the INJECT
                                // the 1b ancestor re-infected its relatives with now costs what COPY
                                // does and lands only through wounds, docs/bites-plan.md)
                                "seed.genome = NOP0 ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB "
                                "SENSE DIVIDE NOP0\n"
                                "seed.at = 1000\n"
                                "seed.millicores = 200\n"
                                "hazard.copy = 0.005\n"
                                "hazard.flaw = 0.001\n"
                                "hazard.rot = 0.00005\n"
                                // (default heat: at kSoupWorld1b's hot gains the COPY replicator
                                // can't establish on this table; the INJECT one it replaced did,
                                // when INJECT was nearly free)
                                "cost.table = home_v1\n";
const char* timed_world() { return kTimedWorld; }

void test_determinism() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    u64 started = 0, landed = 0, failed = 0, defended = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, timed_world(), threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            Rng r(99, 0, 0);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 200; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = run.world().last_soup_tick();
                started += s.actions_started;
                landed += s.actions_landed;
                failed += s.actions_failed;
                defended += s.actions_defended;
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    printf("  home_v1 world: 200 ticks, %llu actions started, %llu landed, %llu failed (%llu defended), hash %016llx\n",
           static_cast<unsigned long long>(started), static_cast<unsigned long long>(landed),
           static_cast<unsigned long long>(failed), static_cast<unsigned long long>(defended),
           static_cast<unsigned long long>(hashes[0]));
    CHECK(started > 0 && landed > 0);
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);

    // A snapshot with actions in progress.
    Run a;
    CHECK(make_run(a, timed_world(), 4));
    u64 at = 0;
    usize pending = 0;
    for (u64 t = 50; t <= 150 && pending == 0; ++t) {
        a.run_until(t);
        pending = 0;
        for (u32 n = 0; n < a.world().node_count(); ++n)
            pending += a.world().node_soup(n).actions().size();
        at = t;
    }
    CHECK(pending > 0);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(200);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
    printf("  snapshot at tick %llu with %llu actions in progress: resumed exactly\n",
           static_cast<unsigned long long>(at), static_cast<unsigned long long>(pending));

    // classic named outright is no key at all; home_v1 is another world.
    Run c1, c2;
    TextBuf ck;
    ck.append(kSoupWorld1b);
    ck.append("cost.table = classic\n");
    CHECK(make_run(c1, kSoupWorld1b, 4) && make_run(c2, ck.c_str(), 4));
    c1.run_until(100);
    c2.run_until(100);
    CHECK_EQ_U64(c2.world().hash(), c1.world().hash());
    Run h;
    CHECK(make_run(h, timed_world(), 4));
    h.run_until(100);
    CHECK(h.world().hash() != c1.world().hash());
}

// The `defended` milestone: once a lineage has met the gate, its members
// making detect.defended attacks fail in time.
void test_defended_milestone() {
    TextBuf t;
    t.append(timed_world());
    t.append("detect.defended = 3\n");
    // (docs/inject-plan.md: INJECT moves a program now, so it no longer
    // links copies; this world's lineages come from its COPY replicator,
    // given the cores to establish.)
    Run run;
    Config::ParseError pe;
    TextBuf err;
    CHECK(run.config().parse(t.c_str(), &pe));
    run.config().set("world.millicores", "2000");
    run.config().set("seed.millicores", "1000");
    CHECK(run.init(4, &err));
    u64 tick = 0;
    while (!run.detection().found && tick < 400)
        run.run_until(++tick);
    CHECK(run.detection().found);
    const u32 n = run.detection().node;
    Lineages* L = run.world().node_lineages(n);
    const Soup& s = run.world().node_soup(n);
    u32 member = kNone;
    const LineageInfo* info = nullptr;
    for (usize k = 0; k < s.body_count() && member == kNone; ++k) {
        const u32 blk = s.body(s.body_at(k)).blocks;
        if (blk == kNone)
            continue; // (a parasite: no block of its own)
        const BlockTag tag = L->tag(blk);
        info = tag.lineage ? L->lineage(tag.lineage) : nullptr;
        if (info && info->detected)
            member = blk;
    }
    CHECK(member != kNone);
    if (member == kNone)
        return;
    if (info->defences >= 3) { // it defended itself already, by itself
        CHECK(run.defended().found);
        return;
    }
    const u32 left = 3 - info->defences;
    const u64 before = L->defended_detections();
    for (u32 i = 1; i < left; ++i)
        L->on_defended(s, member, tick);
    CHECK(L->defended_detections() == before);
    L->on_defended(s, member, tick);
    CHECK(L->defended_detections() == before + 1);
    bool event = false;
    for (usize i = 0; i < L->gates().size(); ++i)
        event = event || L->gates()[i].kind == kEventDefended;
    CHECK(event);
    run.run_until(tick + 1); // the run drains it
    CHECK(run.defended().found);
}

// --- Phase 9b: the GPU and NPU ----------------------------------------------

// A quiet soup with warps on home_v1.
struct GFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\nworld.gpus = 400\ngpu.warps = true\ncost.table = home_v1\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
    // A body born on the GPU running prog at `at`, holding `gpus` GPU units.
    u32 plant_gpu(u32 at, const char* prog, u32 gpus) {
        Array<u8> code;
        TextBuf err;
        if (!genome_from_names(p.table, prog, &code, &err))
            return kNone;
        for (usize i = 0; i < code.size(); ++i)
            s.mem_mut()[at + i] = code[i];
        return s.create_body_form(at, static_cast<u32>(code.size()), 0, gpus, kFormGpu);
    }
};

// The table's devices, the warps' times, and what a timed table sets.
void test_device_table() {
    CostTable h, c;
    CHECK(builtin_costs("home_v1", &h) && builtin_costs("classic", &c));
    CHECK(h.gpu_alu == 1393 && h.gpu_thr == 700 && h.gpu_start == 67000000 && h.npu_mac == 2200);
    CHECK(h.gpu_op[static_cast<u32>(Op::div)] == h.gpu_div && h.gpu_op[static_cast<u32>(Op::inc)] == h.gpu_alu);
    CHECK(h.gpu_op[static_cast<u32>(Op::copy)] == h.gpu_mem + h.gpu_alu);
    CHECK(h.gpu_op[static_cast<u32>(Op::push)] == h.gpu_l1 && h.gpu_op[kOpCount] == h.gpu_alu);
    CHECK(c.gpu_op[0] == 0 && c.gpu_start == 0);
    SoupParams p;
    CHECK(make_params(&p,
                      "world.millicores = 400\nworld.gpus = 64\nworld.npus = 64\ngpu.warps = true\n"
                      "cost.table = home_v1\ncpu.instr_per_core = 100\n",
                      1024));
    CHECK(p.warp_capacity == 19000 && p.warp_step_cost == 32 * 700 && p.warp_lane_cost == 0);
    CHECK(p.gpu_elements == 19000 / 700 && p.npu_macs == 1);
}

// A warp's own clock: a tick (19,000 ps) of a lane's adds (1,393 ps) is 13
// steps. With few units the lanes' throughput binds instead: 8 units give
// 8 x 19,000 of lane-time, and a step fills the warp's 32 lanes at 700 ps.
void test_warp_clock() {
    GFix f;
    CHECK(f.init("cpu.instr_per_core = 100\n"));
    const u32 b = f.plant_gpu(100, "INC INC INC INC", 64);
    CHECK(b != kNone && f.s.warp_count(b) == 1);
    const u32 w = f.s.warp_head(b);
    f.s.tick(0);
    CHECK(f.s.warp(w).steps == 19000 / 1393);
    f.s.tick(1);
    CHECK(f.s.warp(w).steps == 2 * (19000 / 1393));

    GFix g;
    CHECK(g.init("cpu.instr_per_core = 100\n"));
    const u32 b2 = g.plant_gpu(100, "INC INC INC INC", 8);
    g.s.tick(0);
    CHECK(g.s.warp(g.s.warp_head(b2)).steps == 7); // 6 x 22,400 < 152,000 <= 7 x 22,400
    CHECK(soup_ok(f.s) && soup_ok(g.s));
}

// docs/lanes-plan.md L3: a step costs the warp's own lanes, not its node's
// width. On a node of 256 lanes, with 8 units each (152,000 of lane-time): a
// 16-lane warp's step is 11,200, so its clock binds first (13 steps, as
// above); a 256-lane warp's is 179,200, so it gets one step (overshooting).
void test_warp_own_lanes_cost() {
    SoupParams p;
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 2000\nworld.gpus = 400\ngpu.warps = true\ncost.table = home_v1\n"
               "cpu.instr_per_core = 100\nwarp.lanes_min = 16\nwarp.lanes_max = 256\n");
    CHECK(make_params(&p, cfg.c_str(), 1024));
    set_warp_lanes(&p, 256); // a node of 256 lanes, as World::derive_node draws one
    Soup s;
    s.init(&p, 23, 0);
    Array<u8> code;
    TextBuf err;
    TextBuf big;
    for (u32 i = 0; i < 200; ++i)
        big.append(i ? " INC" : "INC");
    CHECK(genome_from_names(p.table, "INC INC INC INC", &code, &err));
    for (usize i = 0; i < code.size(); ++i)
        s.mem_mut()[100 + i] = code[i];
    const u32 small = s.create_body_form(100, 4, 0, 8, kFormGpu);
    CHECK(genome_from_names(p.table, big.c_str(), &code, &err));
    for (usize i = 0; i < code.size(); ++i)
        s.mem_mut()[300 + i] = code[i];
    const u32 wide = s.create_body_form(300, 200, 0, 8, kFormGpu);
    CHECK(small != kNone && wide != kNone);
    const u32 ws = s.warp_head(small), ww = s.warp_head(wide);
    CHECK(s.warp_lanes(ws) == 16 && s.warp_lanes(ww) == 256);
    CHECK(s.warp_step_price(16) == 16 * 700 && s.warp_step_price(256) == 256 * 700);
    s.tick(0);
    CHECK(s.warp(ws).steps == 19000 / 1393 && s.warp(ws).lane_instr == 16 * (19000 / 1393));
    CHECK(s.warp(ww).steps == 1 && s.warp(ww).lane_instr == 256);
    CHECK(soup_ok(s));
}

// A host instruction: the warp waits a GPU round trip and the host's time,
// (67 us + a kernel call) / 4.75 us a tick: 15 ticks.
void test_host_wait() {
    GFix f;
    CHECK(f.init("cpu.instr_per_core = 25000\n"));
    const u32 b = f.plant_gpu(100, "ALLOC INC INC INC INC INC INC INC", 64);
    const u32 w = f.s.warp_head(b);
    for (u32 l = 0; l < f.p.warp_lanes; ++l) {
        Lane& x = f.s.lane_mut(w, l);
        x.bx = 0; // memory
        x.cx = 4;
    }
    f.s.tick(0);
    CHECK(f.s.tally().warp_host == 1 && f.s.warp(w).steps == 0);
    for (u64 t = 1; t <= 14; ++t) {
        f.s.tick(t);
        CHECK(f.s.warp(w).steps == 0);
    }
    f.s.tick(15);
    CHECK(f.s.warp(w).steps > 0);
    CHECK(soup_ok(f.s));
}

// A warp's ENGULF is an action too: begun at the host, landed at its end.
void test_warp_engulf() {
    GFix f;
    CHECK(f.init("cpu.instr_per_core = 25000\n"));
    const u32 eater = f.plant_gpu(100, "ENGULF NOP0 NOP0 NOP1 INC", 64);
    const u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 INC", 300);
    CHECK(eater != kNone && prey != kNone);
    f.s.tick(0);
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].warp == 1 && f.s.body(prey).alive);
    CHECK(f.s.action_on(prey) != kNone);
    f.s.tick(1);
    CHECK(!f.s.body(prey).alive && f.s.tally().actions_landed == 1 && f.s.tally().engulfs == 1);
    CHECK(f.s.actions().empty() && soup_ok(f.s));
}

// A block op in instant mode: the first of a tick pays the GPU's start-up
// (67 us), so the CPU waits 15 ticks for it; it is done at the end.
void test_block_op_action() {
    Fix f;
    CHECK(f.init("world.gpus = 400\ncpu.instr_per_core = 25000\n"));
    const u32 b = plant(f.s, 100, "GPU INC INC INC", 500);
    CHECK(b != kNone && f.s.claim_gpus(b, 64) == 64);
    f.s.prepare_gpu();
    Cpu& c = f.cpu(b);
    c.bx = 0; // a compare
    c.cx = 4;
    f.s.run_cpu(f.cpu_of(b), 100000000);
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].op == static_cast<u32>(Op::gpu));
    CHECK(f.s.actions()[0].end == 15);
    f.s.begin_tick(15);
    f.s.prepare_gpu();
    f.s.run_cpu(f.cpu_of(b), 0);
    CHECK(f.s.actions().empty() && f.s.tally().actions_landed + f.s.tally().actions_failed == 1);
    CHECK(soup_ok(f.s));
}

// A busy warp world on home_v1: the same at 1, 4 and 20 threads, under
// shuffled dispatch, through a snapshot, and on the real GPU (every round
// checked against the reference) and split between the GPU and the CPU.
const char* timed_warp_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append("seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 16\nworld.soup_size = 2048\n"
             "world.millicores = 300\nworld.gpus = 300\ngpu.warps = true\n"
             "edge.bandwidth = 8\ncpu.instr_per_core = 25000\ncpu.wear = 0.002\ngpu.wear = 0.002\n"
             "spark.timing = last\nspark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.gpu_grant = 6\n"
             "spark.min_len = 8\nspark.max_len = 32\nhazard.copy = 0.005\nhazard.flaw = 0.001\n"
             "hazard.rot = 0.00005\nsalvage.ratio = 4\nworld.disk = 512\nheat.gpu_gain = 30\n"
             "seed.at = 600\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 64\n"
             "seed.genome = SENSE ALLOC SHR SHR SHR SHR SHR NOP1 COPY DJNZ NOP0 ZERO INC INC ROTB "
             "ZERO INC SHL SHL SHL ALLOC ZERO ROTB INC SHL SHL SHL SHL SHL DIVIDE ZERO ZERO\n"
             "cost.table = home_v1\n");
    return t.c_str();
}

void test_timed_warps() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    u64 steps = 0, host = 0, started = 0;
    for (u32 v = 0; v < 4; ++v) {
        TextBuf cfg;
        cfg.append(timed_warp_world());
        cfg.append("warp.device = cpu\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            Rng r(99, 0, 0);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 200; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = run.world().last_soup_tick();
                steps += s.warp_steps;
                host += s.warp_host;
                started += s.actions_started;
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    printf("  home_v1 warp world: 200 ticks, %llu warp steps, %llu host instructions, %llu actions, hash %016llx\n",
           static_cast<unsigned long long>(steps), static_cast<unsigned long long>(host),
           static_cast<unsigned long long>(started), static_cast<unsigned long long>(hashes[0]));
    CHECK(steps > 0 && host > 0);
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);

    TextBuf cc;
    cc.append(timed_warp_world());
    cc.append("warp.device = cpu\n");
    Run a;
    CHECK(make_run(a, cc.c_str(), 4));
    a.run_until(100);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(200);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);

    // The real GPU: every round checked word for word; and split at random.
    const char* const devices[] = {"warp.device = gpu\nwarp.verify = 1\n", "warp.device = mixed\nwarp.verify = 3\n"};
    for (const char* d : devices) {
        TextBuf g;
        g.append(timed_warp_world());
        g.append(d);
        Run run;
        CHECK(make_run(run, g.c_str(), 4));
        if (strcmp(run.world().warp_device_name(), "cpu") == 0) {
            printf("  (no GPU: the device runs skipped)\n");
            break;
        }
        run.run_until(200);
        const World& w = run.world();
        printf("  home_v1 warps on %s: %llu rounds on the GPU, %llu verified, %llu differed\n", d,
               static_cast<unsigned long long>(w.warp_gpu_rounds()), static_cast<unsigned long long>(w.warp_verified()),
               static_cast<unsigned long long>(w.warp_mismatches()));
        CHECK(w.warp_gpu_rounds() > 0 && w.warp_mismatches() == 0 && !w.warp_failed());
        CHECK_EQ_U64(w.hash(), hashes[0]);
    }
}

// --- Phase 9c: the fixes ------------------------------------------------------

// C3: a CPU waiting on the disk is idle -- its units do no work -- and its
// share of the body's time goes to the body's other CPU.
void test_idle_wait() {
    Fix f;
    CHECK(f.init("world.disk = 4096\ncpu.instr_per_core = 25000\n"));
    const u32 a = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 ALLOC INC INC INC INC INC INC INC", 1000);
    const u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 DISK INC INC INC INC", 500);
    CHECK(a != kNone && prey != kNone);
    f.s.run_cpu(f.cpu_of(a), 1000); // ENGULF: an action, one tick
    f.s.begin_tick(1);
    f.s.run_cpu(f.cpu_of(a), 0); // it lands: the prey's CPU is the eater's now
    const u32 c1 = f.cpu_of(a), c2 = f.s.cpu(c1).next;
    CHECK(!f.s.body(prey).alive && c2 != kNone && f.s.body(a).ncpu == 2);
    Cpu& x = f.s.cpu_mut(c1);
    x.bx = 3; // ALLOC: disk
    x.cx = 16;
    f.s.run_cpu(c1, 1);
    CHECK(f.s.disk_held(a) == 16);
    Cpu& y = f.s.cpu_mut(c2); // the other CPU stores to it: a 12 us write, three ticks
    y.ip = 203;
    y.bx = 1;
    y.ax = 0;
    y.cx = 16;
    y.rh = 200;
    f.s.run_cpu(c2, 100000000);
    CHECK(f.s.actions().size() == 1 && f.s.cpu_waits(c2));
    // A whole tick: the waiting CPU runs nothing, the other has all 1.5 cores.
    const u64 before = f.s.cpu(c2).executed;
    f.s.tick(2);
    CHECK(f.s.cpu(c2).executed == before && f.s.cpu_waits(c2));
    const double budget = 1500.0 * f.p.tick_units / 1000.0; // heat off: a unit's speed is 1
    CHECK(static_cast<double>(f.s.body(a).work) >= 0.95 * budget);

    // Alone and waiting, a CPU's units do no work at all: they cool.
    Fix g;
    CHECK(g.init("world.disk = 4096\ncpu.instr_per_core = 25000\n"));
    const u32 b = plant(g.s, 100, "ALLOC DISK INC INC INC INC INC INC", 1000);
    Cpu& c = g.cpu(b);
    c.bx = 3;
    c.cx = 16;
    g.s.run_cpu(g.cpu_of(b), 1);
    c.bx = 1;
    c.ax = 0;
    c.cx = 16;
    c.rh = 100;
    CHECK(g.s.run_cpu(g.cpu_of(b), 100000000) < 100000000); // the rest of the tick isn't work
    g.s.tick(1);
    CHECK(g.s.body(b).work == 0 && g.s.actions().size() == 1);
    CHECK(soup_ok(f.s) && soup_ok(g.s));
}

// C1: a warp's searching step costs its template codons and the places its
// slowest lane looked at, at gpu_scan each: JMP NOP0 past ten INCs to the
// NOP1 reads 1 codon and looks at 11 places. A step that runs past the
// tick's end is carried into the next.
void test_warp_search_clock() {
    GFix f;
    CHECK(f.init("cpu.instr_per_core = 25000\n"));
    const u32 b = f.plant_gpu(100, "JMP NOP0 INC INC INC INC INC INC INC INC INC INC NOP1", 64);
    const u32 w = f.s.warp_head(b);
    f.s.tick(0);
    const double tick = f.p.tick_units;
    const double step = static_cast<double>(f.p.cost.gpu_alu + 12 * f.p.cost.gpu_scan);
    const u64 want = static_cast<u64>((tick - static_cast<double>(f.p.cost.gpu_alu)) / step) + 1;
    CHECK(f.s.warp(w).steps == want);
    f.s.tick(1); // tick 0's overshoot comes off tick 1
    CHECK(f.s.warp(w).steps < 2 * want);
    CHECK(soup_ok(f.s));
}

// C2: async results come back after each device's own round trip: the GPU's
// 67 us is 15 ticks of 4.75 us, the NPU's 3.37 ms is 710; classic keeps
// accel.latency.
void test_async_latency() {
    SoupParams p;
    CHECK(make_params(&p,
                      "world.millicores = 400\nworld.gpus = 64\nworld.npus = 64\naccel.mode = async\n"
                      "cost.table = home_v1\ncpu.instr_per_core = 25000\n",
                      1024));
    CHECK(p.gpu_latency == 15 && p.npu_latency == 710);
    SoupParams q;
    CHECK(make_params(&q, "world.gpus = 64\naccel.mode = async\naccel.latency = 3\n", 1024));
    CHECK(q.gpu_latency == 3 && q.npu_latency == 3);

    Fix f;
    CHECK(f.init("world.gpus = 400\naccel.mode = async\ncpu.instr_per_core = 25000\n"));
    const u32 b = plant(f.s, 100, "GPU INC INC INC", 500);
    CHECK(f.s.claim_gpus(b, 64) == 64);
    f.s.prepare_gpu();
    Cpu& c = f.cpu(b);
    c.bx = 0; // a compare
    c.cx = 4;
    f.s.run_cpu(f.cpu_of(b), 1);
    Array<AccelOp> ops;
    Array<u8> in;
    Array<u32> addr;
    f.s.accel_gather(&ops, &in, &addr);
    CHECK(ops.size() == 1 && ops[0].due == 14); // issued at tick 0, landing at the barrier of tick 14
}

// --- Phase 9c-b1: threads -------------------------------------------------------

// A quiet soup with threads on, the classic table (so an instruction is 1),
// 2,000 millicores (two cores).
struct TFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra = "") {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\ncpu.threads = true\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
};

// One thread on two cores' millicores runs at one core's speed; without
// threads it ran at two.
void test_thread_core_cap() {
    TFix f;
    CHECK(f.init("cpu.instr_per_core = 100\n"));
    const u32 b = plant(f.s, 100, "INC INC INC INC", 2000);
    f.s.tick(0);
    CHECK(f.s.cpu(f.s.body(b).cpus).executed == 100);
    SoupParams p;
    CHECK(make_params(&p, "heat.enabled = false\nmem.upkeep = 0\nworld.millicores = 2000\ncpu.instr_per_core = 100\n",
                      1024));
    Soup s;
    s.init(&p, 23, 0);
    s.begin_tick(0);
    const u32 b2 = plant(s, 100, "INC INC INC INC", 2000);
    s.tick(0);
    CHECK(s.cpu(s.body(b2).cpus).executed == 200);
}

// BX 21 starts a thread at the code after its template's match, with the
// starter's registers and heads; it runs from the next tick, and both then
// run a core each. cpu.max_threads stops more.
void test_thread_start() {
    TFix f;
    CHECK(f.init("cpu.instr_per_core = 100\ncpu.max_threads = 2\n"));
    // 0 GPU NOP1x3 (start a thread at the NOP0x3 below), then the starter's
    // INC loop; the new thread's own INC loop after the NOP0s.
    const u32 b = plant(f.s, 100,
                        "GPU NOP1 NOP1 NOP1 ZERO INC INC INC NOP0 NOP0 NOP0 SWAP SWAP SWAP SWAP SWAP SWAP", 2000);
    CHECK(b != kNone);
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.bx = kThreadOp;
    c.ax = 7;
    c.rh = 130;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.body(b).ncpu == 2 && f.s.tally().threads_started == 1);
    const u32 t = f.s.cpu(f.s.body(b).cpus).next;
    CHECK(t != kNone && f.s.cpu(t).ip == 111 && f.s.cpu(t).ax == 7 && f.s.cpu(t).rh == 130);
    CHECK(f.s.cpu(f.s.body(b).cpus).ip == 104 && f.s.cpu(f.s.body(b).cpus).err == 0);
    // A third: the limit.
    Cpu& c2 = f.s.cpu_mut(f.s.body(b).cpus);
    c2.ip = 100;
    c2.bx = kThreadOp;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.body(b).ncpu == 2 && f.s.cpu(f.s.body(b).cpus).err == 1);
    // A tick: each runs a core's worth.
    const u64 e0 = f.s.cpu(f.s.body(b).cpus).executed, e1 = f.s.cpu(t).executed;
    f.s.tick(1);
    CHECK(f.s.cpu(f.s.body(b).cpus).executed - e0 == 100 && f.s.cpu(t).executed - e1 == 100);
    CHECK(soup_ok(f.s));
}

// BX 22 sets the kind of core a thread prefers; SENSE 24 counts my threads
// and 25 reads my kind.
void test_thread_kind() {
    TFix f;
    CHECK(f.init());
    const u32 b = plant(f.s, 100, "GPU SENSE SENSE INC", 500);
    const u32 c = f.s.body(b).cpus;
    Cpu& x = f.s.cpu_mut(c);
    x.bx = kKindOp;
    x.ax = 1;
    f.s.run_cpu(c, 1);
    CHECK(f.s.cpu_kind(c) == kKindE && f.s.cpu(c).err == 0);
    x.bx = 25;
    f.s.run_cpu(c, 1);
    CHECK(f.s.cpu(c).cx == kKindE);
    x.bx = 24;
    f.s.run_cpu(c, 1);
    CHECK(f.s.cpu(c).cx == 1);
}

// Threads take turns a slice at a time, so one sees another's writes within
// a tick. The starter writes 1, 2, 3 ... into a buffer; the thread it
// started (first in tick 1's order) idles 7 instructions, then reads the
// buffer's 21st codon. In 8 slices the starter has written it in slice 0
// (21); straight through, it hasn't yet (the buffer's filler, ZERO's codon).
u32 slice_read(u32 slices) {
    TFix f;
    TextBuf extra;
    extra.append("cpu.instr_per_core = 60\ncpu.slices = ");
    extra.append_u64(slices);
    extra.append_char('\n');
    if (!f.init(extra.c_str()))
        return kNone;
    // After its READ the thread spins on its own JMP (the ZERO at 129 ends that
    // JMP's template, so the buffer's codons can't join it).
    TextBuf code;
    code.append("GPU NOP1 NOP1 NOP1 ZERO NOP1 NOP0 INC WRITE JMP NOP0 NOP1 ZERO NOP0 NOP0 NOP0 "
                "SWAP SWAP SWAP SWAP SWAP SWAP SWAP READ NOP0 NOP0 JMP NOP1 NOP1 ZERO");
    for (int i = 0; i < 64; ++i)
        code.append(" ZERO"); // the buffer, at 130
    const u32 b = plant(f.s, 100, code.c_str(), 2000);
    if (b == kNone)
        return kNone;
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.bx = kThreadOp;
    c.wh = 130;      // the starter writes from the buffer's start
    c.rh = 130 + 20; // the started thread reads its 21st codon
    f.s.tick(0);
    f.s.tick(1);
    const u32 t = f.s.cpu(f.s.body(b).cpus).next;
    return t == kNone ? kNone : f.s.cpu(t).cx;
}

void test_thread_slices() {
    SoupParams p;
    CHECK(make_params(&p, "world.millicores = 2000\n", 1024));
    const u32 zero = static_cast<u32>(codon_of(p.table, Op::zero));
    CHECK(slice_read(8) == 21);
    CHECK(slice_read(1) == zero);
}

// A busy world with threads on: deterministic at 1, 4 and 20 threads, under
// shuffled dispatch and through a snapshot.
void test_thread_world() {
    TextBuf t;
    t.append(kSoupWorld1b);
    t.append("cpu.threads = true\n");
    u64 hashes[3] = {};
    const u32 threads[3] = {1, 4, 20};
    for (u32 v = 0; v < 3; ++v) {
        Run run;
        CHECK(make_run(run, t.c_str(), threads[v]));
        run.run_until(200);
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    Run a;
    CHECK(make_run(a, t.c_str(), 4));
    a.run_until(100);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(200);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
    Run off;
    CHECK(make_run(off, kSoupWorld1b, 4));
    off.run_until(200);
    CHECK(off.world().hash() != hashes[0]); // threads change the world
    printf("  threads world: 200 ticks, hash %016llx\n", static_cast<unsigned long long>(hashes[0]));
}

// --- Phase 9c-b2: E cores ---------------------------------------------------------

// A quiet soup on home_v1 with 2,000 millicores, cpu.e_share of them E.
struct EFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\ncost.table = home_v1\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
};

// The E column: home_v1's E-core figures for the CPU's operations, the
// P-core's for the rest; classic is classic.
void test_e_table() {
    CostTable p = {}, e = {}, c = {};
    CHECK(builtin_costs("home_v1", &p) && builtin_costs_e("home_v1", &e));
    CHECK(e.timed == 1 && e.alu == 218 && e.mul == 655 && e.div == 4425 && e.load == 872 && e.store == 109);
    CHECK(e.jump == 4996 && e.scan == 437 && e.move == 92 && e.draw == 3489 && e.call == 631000);
    CHECK(e.share == p.share && e.disk_read == p.disk_read && e.disk_write_codon == p.disk_write_codon);
    CHECK(e.gpu_start == p.gpu_start && e.gpu_op[static_cast<u32>(Op::inc)] == p.gpu_op[static_cast<u32>(Op::inc)]);
    CHECK(e.op[static_cast<u32>(Op::inc)] == 218 && e.op[static_cast<u32>(Op::copy)] == e.load + e.store);
    CHECK(builtin_costs_e("classic", &c) && !c.timed && c.op[0] == 1);
    TextBuf d;
    describe_costs(p, d);
    CHECK(strstr(d.c_str(), "on an E-core") != nullptr);
    // E millicores need a timed table (an E-core's speed is its column).
    SoupParams sp;
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse("cpu.e_share = 0.5\n", &pe));
    CHECK(!soup_params_from_config(cfg, 1024, &sp));
}

// R1: the last round(millicores x e_share) units are E, in a pool of their
// own; a new body draws from both pools in proportion to what's free; ALLOC
// and RELEASE take E millicores with BX 5 (BX 1 is P); SENSE 26 and 27 read
// mine and the node's free ones.
void test_e_units() {
    EFix f;
    CHECK(f.init("cpu.e_share = 0.25\n"));
    CHECK(f.p.sel.res_n == 6 && f.p.sel.sense_n == kSenseReadingsThreads);
    CHECK(f.s.e_total() == 500 && f.s.free_e_units() == 500 && f.s.free_units() == 2000);
    const u32 b = plant(f.s, 100, "ALLOC ALLOC RELEASE RELEASE SENSE SENSE", 1000);
    CHECK(b != kNone && f.s.body(b).units == 1000 && f.s.e_units(b) == 250); // 1500:500 free
    const u32 c = f.s.body(b).cpus;
    Cpu& x = f.s.cpu_mut(c);
    x.bx = 5; // ALLOC E millicores
    x.cx = 100;
    f.s.run_cpu(c, 1);
    CHECK(x.err == 0 && f.s.e_units(b) == 350 && f.s.free_e_units() == 150);
    x.bx = 1; // ALLOC P millicores
    f.s.run_cpu(c, 1);
    CHECK(f.s.body(b).units == 1200 && f.s.e_units(b) == 350);
    x.bx = 5; // RELEASE E
    x.cx = 50;
    f.s.run_cpu(c, 1);
    CHECK(f.s.e_units(b) == 300 && f.s.body(b).units == 1150);
    x.bx = 1; // RELEASE P: only P, all 850 of them
    x.cx = 1000;
    f.s.run_cpu(c, 1);
    CHECK(x.err == 0 && f.s.body(b).units == 300 && f.s.e_units(b) == 300);
    x.bx = 26;
    f.s.run_cpu(c, 1);
    CHECK(x.cx == 300);
    x.bx = 27;
    f.s.run_cpu(c, 1);
    CHECK(x.cx == 200);
    CHECK(soup_ok(f.s));
    // Without E millicores BX 5 fails.
    EFix g;
    CHECK(g.init("cpu.threads = true\n"));
    const u32 b2 = plant(g.s, 100, "ALLOC", 100);
    Cpu& y = g.s.cpu_mut(g.s.body(b2).cpus);
    y.bx = 5;
    y.cx = 10;
    g.s.run_cpu(g.s.body(b2).cpus, 1);
    CHECK(y.err == 1 && g.s.e_total() == 0);
}

// R3: an instruction on an E-core costs the E column's time, so a body all of
// E millicores runs fewer adds in a tick; its time on them is e_work.
void test_e_costs() {
    EFix f;
    CHECK(f.init("cpu.e_share = 1\ncpu.instr_per_core = 100\n"));
    const u32 b = plant(f.s, 100, "INC INC INC INC", 1000);
    CHECK(f.s.e_units(b) == 1000);
    f.s.tick(0);
    // A core's tick is 100 P-core adds (19,000 ps); an E-core add is 218 ps.
    CHECK(f.s.cpu(f.s.body(b).cpus).executed == 88 && f.s.tally().e_work == 88u * 218u);
    EFix g;
    CHECK(g.init("cpu.e_share = 0.5\n"));
    const u32 b2 = plant(g.s, 100, "MUL MUL", 100);
    CHECK(g.s.run_cpu(g.s.body(b2).cpus, 1, kKindE) == g.p.ecost.mul && g.p.ecost.mul == 655);
    CHECK(g.s.run_cpu(g.s.body(b2).cpus, 1, kKindP) == g.p.cost.mul);
}

// Two threads (the starter and the one it starts) looping on INCs in a body
// of 1,000 P and 1,000 E millicores, preferring kinds k0 and k1: each one's
// time in a tick, and its part on E.
void two_threads(u8 k0, u8 k1, bool threads, u32 time[2], u32 etime[2]) {
    EFix f;
    time[0] = time[1] = etime[0] = etime[1] = 0;
    if (!f.init(threads ? "cpu.e_share = 0.5\ncpu.instr_per_core = 25000\ncpu.threads = true\n"
                        : "cpu.e_share = 0.5\ncpu.instr_per_core = 25000\n"))
        return;
    TextBuf code;
    code.append("GPU NOP1 NOP1 NOP1 ZERO NOP0 NOP0 NOP0 NOP1 NOP0");
    for (int i = 0; i < 60; ++i)
        code.append(" INC");
    code.append(" JMP NOP0 NOP1"); // back to the INCs at 110
    const u32 b = plant(f.s, 100, code.c_str(), 2000);
    if (b == kNone || f.s.e_units(b) != 1000)
        return;
    const u32 c0 = f.s.body(b).cpus;
    u32 cs[2] = {c0, kNone};
    if (threads) {
        f.s.cpu_mut(c0).bx = kThreadOp;
        f.s.run_cpu(c0, 1);
        cs[1] = f.s.cpu(c0).next;
        if (cs[1] == kNone)
            return;
        const u8 kinds[2] = {k0, k1};
        for (u32 i = 0; i < 2; ++i) { // BX 22, then on to the INCs
            Cpu& x = f.s.cpu_mut(cs[i]);
            x.ip = 100;
            x.bx = kKindOp;
            x.ax = kinds[i];
            f.s.run_cpu(cs[i], 1);
            x.ip = 110;
        }
    }
    f.s.tick(0);
    for (u32 i = 0; i < 2; ++i)
        if (cs[i] != kNone) {
            time[i] = f.s.cpu_time(cs[i]);
            etime[i] = f.s.cpu_etime(cs[i]);
        }
    CHECK(soup_ok(f.s));
}

// R3: each thread first gets up to a core of its preferred kind, then the
// other kind's leftovers, with no penalty: a thread never idles while its
// body has time of either kind.
void test_e_share_time() {
    const i64 core = 25000 * 190; // a core's tick, ps
    // within the last instruction's overshoot (a JMP's search, on an E-core)
    auto near = [](u32 v, i64 want) {
        const i64 d = static_cast<i64>(v) - want;
        return d > -40000 && d < 40000;
    };
    u32 t[2], e[2];
    two_threads(kKindP, kKindE, true, t, e); // a core of each kind, one each
    CHECK(near(t[0], core) && e[0] == 0 && near(t[1], core) && near(e[1], core));
    two_threads(kKindP, kKindP, true, t, e); // P over-subscribed: half a core of P each, the E core's time shared
    CHECK(near(t[0], core) && near(t[1], core));
    CHECK(near(e[0], core / 2) && near(e[1], core / 2));
    two_threads(kKindE, kKindE, true, t, e); // the same the other way
    CHECK(near(t[0], core) && near(t[1], core));
    CHECK(near(e[0], core / 2) && near(e[1], core / 2));
    two_threads(kKindP, kKindP, false, t, e); // no threads: one CPU runs both cores' time
    CHECK(near(t[0], 2 * core) && near(e[0], core));
}

// R3: heat lands on the units that ran the work, E at heat.e_gain x P's gain.
void test_e_heat() {
    SoupParams p;
    CHECK(make_params(&p,
                      "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\nworld.millicores = 2000\n"
                      "cost.table = home_v1\ncpu.e_share = 0.5\ncpu.instr_per_core = 100\n",
                      1024));
    Soup s;
    s.init(&p, 23, 0);
    s.begin_tick(0);
    const u32 b = plant(s, 100, "INC INC INC INC", 2000);
    CHECK(b != kNone && s.e_units(b) == 1000);
    s.tick(0);
    // Both kinds flat out: a P unit takes cpu_gain (6), an E unit 0.4 x 6.
    float hp = -1, he = -1;
    for (u32 u = 0; u < 2000; ++u) {
        if (s.unit_owner(u) != b)
            continue;
        if (u < 1000)
            hp = s.unit_heat(u);
        else
            he = s.unit_heat(u);
    }
    CHECK(hp > 5.999f && hp < 6.001f);
    CHECK(he > 2.399f && he < 2.401f);
    CHECK(s.tally().e_work == 88u * 218u);
}

// R6: DIVIDE gives a share of each kind and the divider's kind; ENGULF takes
// both kinds with the prey; a dispersing daughter carries both, and her
// divider's kind, and draws them on arrival.
void test_e_moves() {
    EFix f;
    CHECK(f.init("cpu.e_share = 0.5\ncpu.threads = true\n"));
    const u32 b = plant(f.s, 100, "GPU ALLOC DIVIDE", 2000);
    CHECK(f.s.e_units(b) == 1000);
    const u32 c = f.s.body(b).cpus;
    Cpu& x = f.s.cpu_mut(c);
    x.bx = kKindOp;
    x.ax = 1;
    f.s.run_cpu(c, 1); // prefers E
    x.bx = 0;          // ALLOC 16 codons of memory
    x.cx = 16;
    f.s.run_cpu(c, 1);
    x.bx = 0; // DIVIDE here, half of each
    x.cx = 32;
    f.s.run_cpu(c, 1);
    CHECK(x.err == 0 && f.s.tally().births == 1);
    u32 d = kNone;
    for (usize k = 0; k < f.s.body_count(); ++k)
        if (f.s.body_at(k) != b)
            d = f.s.body_at(k);
    CHECK(d != kNone && f.s.body(d).units == 1000 && f.s.e_units(d) == 500 && f.s.e_units(b) == 500);
    CHECK(f.s.cpu_kind(f.s.body(d).cpus) == kKindE);
    CHECK(soup_ok(f.s));

    // ENGULF: the prey's E millicores go with it.
    EFix g;
    CHECK(g.init("cpu.e_share = 0.5\n"));
    const u32 eater = plant(g.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    const u32 prey = plant(g.s, 200, "NOP1 NOP1 NOP0 SENSE", 300);
    CHECK(eater != kNone && prey != kNone);
    const u32 ee = g.s.e_units(eater), pe = g.s.e_units(prey);
    CHECK(ee == 250 && pe == 150);
    g.s.run_cpu(g.s.body(eater).cpus, 1000000);
    g.s.begin_tick(1);
    g.s.run_cpu(g.s.body(eater).cpus, 0);
    CHECK(g.s.tally().engulfs == 1 && g.s.e_units(eater) == ee + pe && g.s.body(eater).units == 800);
    g.s.death_sweep(); // the prey's slot
    CHECK(soup_ok(g.s));

    // Dispersal: her shares leave at home and are drawn on arrival.
    EFix h;
    CHECK(h.init("cpu.e_share = 0.5\ncpu.threads = true\nworld.dispersal = true\n"));
    const u32 edges[1] = {0}, flight[1] = {0};
    DispersalPorts ports;
    ports.edges = edges;
    ports.in_flight = flight;
    ports.degree = 1;
    ports.capacity = 4;
    h.s.set_ports(ports);
    const u32 pb = plant(h.s, 100, "GPU ALLOC DIVIDE", 2000);
    const u32 pc = h.s.body(pb).cpus;
    Cpu& y = h.s.cpu_mut(pc);
    y.bx = kKindOp;
    y.ax = 1;
    h.s.run_cpu(pc, 1);
    y.bx = 0;
    y.cx = 16;
    h.s.run_cpu(pc, 1);
    y.bx = 1; // DIVIDE to neighbour 1, a quarter of each
    y.cx = 16;
    h.s.run_cpu(pc, 1);
    CHECK(y.err == 0 && h.s.departures().size() == 1);
    const Departure dep = h.s.departures()[0];
    CHECK(dep.millicores == 250 && dep.emillicores == 250 && dep.kind == kKindE);
    CHECK(h.s.body(pb).units == 1500 && h.s.e_units(pb) == 750 && h.s.free_e_units() == 250);
    EFix k;
    CHECK(k.init("cpu.e_share = 0.5\ncpu.threads = true\nworld.dispersal = true\n"));
    CargoTag tag = {};
    CHECK(k.s.arrive(h.s.departure_codons() + dep.codon_off, dep.len, dep.millicores, 0, 0, 0, tag, kFormCpu,
                     dep.emillicores, static_cast<u8>(dep.kind)));
    const u32 nb = k.s.body_at(0);
    CHECK(k.s.body(nb).units == 500 && k.s.e_units(nb) == 250 && k.s.cpu_kind(k.s.body(nb).cpus) == kKindE);
    CHECK(soup_ok(h.s) && soup_ok(k.s));
}

// A busy timed world with threads on and half its millicores E: the
// determinism gates (1, 4 and 20 threads, shuffled dispatch, a snapshot);
// work done on E millicores; and the switch off changes the world.
void test_e_world() {
    TextBuf t;
    t.append(kTimedWorld);
    t.append("cpu.threads = true\ncpu.e_share = 0.5\n");
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    u64 ework = 0, started = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, t.c_str(), threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = n - 1 - i;
            run.set_dispatch_order(order.data());
        }
        for (u64 tk = 1; tk <= 200; ++tk) {
            run.run_until(tk);
            if (v == 0) {
                ework += run.world().last_soup_tick().e_work;
                started += run.world().last_soup_tick().threads_started;
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    CHECK(ework > 0);
    Run a;
    CHECK(make_run(a, t.c_str(), 4));
    a.run_until(100);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(200);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
    TextBuf p;
    p.append(kTimedWorld);
    p.append("cpu.threads = true\n");
    Run off;
    CHECK(make_run(off, p.c_str(), 4));
    off.run_until(200);
    CHECK(off.world().hash() != hashes[0]); // E millicores change the world
    printf("  E world: 200 ticks, E work %.3g ms, %llu threads started, hash %016llx\n", ework / 1e9,
           static_cast<unsigned long long>(started), static_cast<unsigned long long>(hashes[0]));
}

// A landscape with E millicores (res.ecpu): sites differ in their share;
// cargo carries both kinds; deterministic, and through a snapshot.
void test_e_landscape() {
    const char* cfg = "seed = 7\n"
                      "world.model = soup\n"
                      "world.graph = landscape\n"
                      "world.nodes = 40\n"
                      "land.scale = 4\n"
                      "land.resolution = 96\n"
                      "world.soup_size = 2048\n"
                      "world.millicores = 600\n"
                      "res.ecpu.spread = 1\n"
                      "res.ecpu.barren = 0.1\n"
                      "cost.table = home_v1\n"
                      "cpu.instr_per_core = 25000\n"
                      "cpu.e_share = 0.4\n"
                      "cpu.threads = true\n"
                      "edge.bandwidth = 8\n"
                      "spark.timing = last\n"
                      "spark.initial = 20\n"
                      "spark.rate = 0.5\n"
                      "spark.grant = 20\n"
                      "spark.min_len = 8\n"
                      "spark.max_len = 24\n"
                      "seed.genome = ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE\n"
                      "seed.at = 600\n"
                      "seed.millicores = 200\n"
                      "heat.cpu_gain = 36\n";
    Run run;
    CHECK(make_run(run, cfg, 4));
    u32 lo = 0xFFFFFFFFu, hi = 0, zero = 0;
    for (u32 n = 0; n < run.world().node_count(); ++n) {
        const Soup& s = run.world().node_soup(n);
        const u32 e = s.params().millicores ? s.e_total() * 1000 / s.params().millicores : 0;
        lo = e < lo ? e : lo;
        hi = e > hi ? e : hi;
        zero += s.e_total() == 0;
    }
    CHECK(hi > lo && hi <= 1000 && zero > 0); // E-rich, E-poor and barren sites
    u64 sent = 0;
    for (u64 tk = 1; tk <= 150; ++tk) {
        run.run_until(tk);
        sent += run.world().last_soup_tick().sent;
    }
    const u64 h = run.world().hash();
    Run one;
    CHECK(make_run(one, cfg, 1));
    one.run_until(80);
    ByteWriter snap;
    one.snapshot(snap);
    Run back;
    TextBuf err;
    CHECK(back.init_from_snapshot(snap.data(), snap.size(), 3, &err));
    back.run_until(150);
    CHECK_EQ_U64(back.world().hash(), h);
    for (u32 n = 0; n < run.world().node_count(); ++n)
        CHECK(soup_ok(run.world().node_soup(n)));
    printf("  E landscape: E share %u..%u per mille, %u sites without, %llu sent in 150 ticks\n", lo, hi, zero,
           static_cast<unsigned long long>(sent));
}

} // namespace

void suite_costs() {
    test_tables();
    test_charges();
    test_budget();
    test_engulf_action();
    test_defence();
    test_actor_lost();
    test_inject_action();
    test_disk_action();
    test_determinism();
    test_defended_milestone();
    // Phase 9b
    test_device_table();
    test_warp_clock();
    test_warp_own_lanes_cost();
    test_host_wait();
    test_warp_engulf();
    test_block_op_action();
    test_timed_warps();
    // Phase 9c
    test_idle_wait();
    test_warp_search_clock();
    test_async_latency();
    // Phase 9c-b1
    test_thread_core_cap();
    test_thread_start();
    test_thread_kind();
    test_thread_slices();
    test_thread_world();
    // Phase 9c-b2
    test_e_table();
    test_e_units();
    test_e_costs();
    test_e_share_time();
    test_e_heat();
    test_e_moves();
    test_e_world();
    test_e_landscape();
}
