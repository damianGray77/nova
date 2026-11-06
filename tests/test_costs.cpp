// Measured costs (Phase 9, design.md 6.15, docs/phase9-plan.md): the cost
// tables, world time charged instruction by instruction, the CPU's time a
// tick, and actions in progress -- ENGULF and INJECT with a start, a wait and
// an end, a target that defends itself by changing in time, an actor lost
// mid-action, a DISK op longer than a tick -- plus the determinism gates and
// the `defended` milestone.
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

// INJECT as an action: its time is the codons it writes, and they land at the end.
void test_inject_action() {
    Fix f;
    CHECK(f.init());
    const u32 inj = plant(f.s, 100, "INJECT NOP0 NOP0 NOP1 INC", 500);
    const u32 host = plant(f.s, 200, "NOP1 NOP1 NOP0 INC INC INC INC INC", 300);
    CHECK(inj != kNone && host != kNone);
    Cpu& c = f.cpu(inj);
    c.cx = 2;
    c.ax = 3;
    c.rh = 100;
    f.s.run_cpu(f.cpu_of(inj), 1000);
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].end == 1);
    const u8 before = f.s.mem()[203];
    f.s.begin_tick(1);
    f.s.run_cpu(f.cpu_of(inj), 0);
    CHECK(f.s.tally().injects == 1 && f.s.tally().actions_landed == 1);
    CHECK(f.s.mem()[203] == f.s.mem()[100] && f.s.mem()[203] != before);
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
                                "seed.genome = NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE INJECT NOP1 DIVIDE\n"
                                "seed.at = 1000\n"
                                "seed.millicores = 100\n"
                                "hazard.copy = 0.005\n"
                                "hazard.flaw = 0.001\n"
                                "hazard.rot = 0.00005\n"
                                "heat.cpu_gain = 36\n"
                                "heat.fetch_gain = 0.06\n"
                                "heat.write_gain = 0.5\n"
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
    Run run;
    CHECK(make_run(run, t.c_str(), 4));
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
    test_host_wait();
    test_warp_engulf();
    test_block_op_action();
    test_timed_warps();
}
