// Phase 8 (design.md 6.14): warps -- programs on the GPU -- and the two forms
// of life. These run on the CPU reference; the GPU's own tests (8b) check it
// gives the very same results.
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/view.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// A hand-written GPU-born replicator, 32 codons (one warp of 32 lanes copies
// it in one step): SENSE its size; ALLOC a block that size (the host moves
// every lane's write head into it, offsets kept); CX = size / 32 = 1 copy
// iteration; COPY (lane L copies codon L); then claim 8 more GPU units
// (BX = 2), put BX back to 0, and DIVIDE with half its holdings. It runs off
// its end back to the start.
const char* const kWarpReplicator = "SENSE ALLOC SHR SHR SHR SHR SHR NOP1 COPY DJNZ NOP0 ZERO INC INC ROTB "
                                    "ZERO INC SHL SHL SHL ALLOC ZERO ROTB INC SHL SHL SHL SHL SHL DIVIDE "
                                    "ZERO ZERO";

// A quiet soup with warps: steps cost 1 work and lanes nothing, so a quota
// counts steps; one GPU unit gives warp.capacity steps a tick.
struct WFix {
    SoupParams p;
    Soup s;
    u32 b = kNone, w = kNone;

    bool init(const char* prog, u32 lanes, u32 steps, const char* extra = "", u32 gpus = 1, u32 at = 100,
              u32 millicores = 0, u32 form = kFormGpu) {
        // kQuiet's settings, each unless `extra` sets it (a key may appear once)
        TextBuf cfg;
        cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                   "world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\n");
        if (!strstr(extra, "mem.upkeep ="))
            cfg.append("mem.upkeep = 0\n");
        if (!strstr(extra, "warp.step_cost"))
            cfg.append("warp.step_cost = 1\n");
        if (!strstr(extra, "warp.lane_cost"))
            cfg.append("warp.lane_cost = 0\n");
        cfg.append("warp.lanes = ");
        cfg.append_u64(lanes);
        cfg.append("\nwarp.capacity = ");
        cfg.append_u64(steps);
        cfg.append_char('\n');
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 5, 0);
        Array<u8> code;
        TextBuf err;
        if (!genome_from_names(p.table, prog, &code, &err)) {
            fprintf(stderr, "  WFix: %s\n", err.c_str());
            return false;
        }
        for (usize i = 0; i < code.size(); ++i)
            s.mem_mut()[at + i] = code[i];
        b = s.create_body_form(at, static_cast<u32>(code.size()), millicores, gpus, form);
        if (b == kNone)
            return false;
        w = s.warp_head(b);
        return true;
    }
    Lane& lane(u32 l) { return s.lane_mut(w, l); }
};

// Lockstep: agreeing lanes run together, one step each instruction.
void test_lockstep() {
    WFix f;
    CHECK(f.init("INC INC INC", 4, 3));
    CHECK(f.s.body(f.b).form == kFormGpu && f.s.warp_count(f.b) == 1 && f.s.body(f.b).ncpu == 0);
    for (u32 l = 0; l < 4; ++l) // heads offset by lane (wrapping inside the 3-codon block)
        CHECK(f.lane(l).ip == 100 && f.lane(l).rh == 100 + l % 3 && f.lane(l).wh == 100 + l % 3);
    f.s.tick(0);
    CHECK(f.s.warp(f.w).steps == 3 && f.s.warp(f.w).lane_instr == 12);
    for (u32 l = 0; l < 4; ++l)
        CHECK(f.lane(l).cx == 3 && f.lane(l).ip == 100); // wrapped at the block's end
    CHECK(f.s.tally().warp_steps == 3 && f.s.tally().lane_instr == 12);
    CHECK(soup_ok(f.s));
}

// Divergence and merging: lanes that branch apart take turns; they merge
// again where their IPs meet.
void test_divergence() {
    WFix f;
    CHECK(f.init("IFZ INC NOP0 NOP0", 4, 3));
    f.lane(1).cx = 1; // lane 1 skips the INC
    f.s.tick(0);
    // step 1: IFZ, 4 lanes; step 2: INC, lanes 0, 2, 3; step 3: NOP0, all 4 again
    CHECK(f.s.warp(f.w).steps == 3 && f.s.warp(f.w).lane_instr == 11);
    CHECK(f.lane(0).cx == 1 && f.lane(1).cx == 1 && f.lane(2).cx == 1 && f.lane(3).cx == 1);
    for (u32 l = 0; l < 4; ++l)
        CHECK(f.lane(l).ip == 103);
}

// Offset heads: a 32-lane COPY copies 32 codons in one step, and the heads
// move on by 32 (wrapping inside the block).
void test_offset_copy() {
    WFix f;
    TextBuf prog;
    prog.append("COPY");
    for (u32 i = 1; i < 64; ++i)
        prog.append(i < 32 ? (i % 2 ? " INC" : " DEC") : " NOP0");
    CHECK(f.init(prog.c_str(), 32, 1));
    for (u32 l = 0; l < 32; ++l)
        f.lane(l).wh = 100 + 32 + l;
    u8 before[32];
    for (u32 i = 0; i < 32; ++i)
        before[i] = f.s.mem()[100 + i];
    f.s.tick(0);
    CHECK(f.s.warp(f.w).steps == 1 && f.s.warp(f.w).lane_instr == 32);
    bool same = true;
    for (u32 i = 0; i < 32; ++i)
        same = same && f.s.mem()[132 + i] == before[i];
    CHECK(same);
    CHECK(f.lane(5).rh == 100 + 37 && f.lane(5).wh == 100 + 5); // +32, the second wrapping at the end
    CHECK(f.lane(5).err == 0 && f.s.tally().copy_errors == 0);
}

// Within a step every lane reads before any writes; the highest lane's
// write to a codon stays.
void test_read_then_write() {
    WFix f;
    TextBuf prog;
    prog.append("COPY");
    for (u32 i = 1; i < 16; ++i)
        prog.append(" NOP0");
    CHECK(f.init(prog.c_str(), 4, 1));
    u8* m = f.s.mem_mut();
    m[110] = 5; // A
    m[111] = 6; // B
    m[112] = 7; // C
    // lane 0: A -> B; lane 1: B -> C (reads B before lane 0 writes it);
    // lanes 2, 3 both write D = 113: lane 3's value stays.
    f.lane(0).rh = 110;
    f.lane(0).wh = 111;
    f.lane(1).rh = 111;
    f.lane(1).wh = 112;
    f.lane(2).rh = 110;
    f.lane(2).wh = 113;
    f.lane(3).rh = 112;
    f.lane(3).wh = 113;
    f.s.tick(0);
    CHECK(m[111] == 5 && m[112] == 6 && m[113] == 7);
}

// A warp's world is its own body: reading waste or another body fails.
void test_own_body() {
    WFix f;
    CHECK(f.init("READ NOP0 NOP0 NOP0", 2, 1));
    f.lane(0).rh = 101;  // its own
    f.lane(1).rh = 500;  // waste
    f.lane(1).cx = 77;
    f.s.tick(0);
    CHECK(f.lane(0).err == 0 && f.lane(1).err == 1 && f.lane(1).cx == 77);
    // and a write into another body fails too
    WFix g;
    CHECK(g.init("WRITE NOP0", 1, 1));
    CHECK(plant(g.s, 300, "NOP0 NOP0", 10) != kNone);
    g.lane(0).wh = 300;
    g.lane(0).cx = 9;
    g.s.tick(0);
    CHECK(g.lane(0).err == 1 && g.s.mem()[300] != 9);
}

// World-changing instructions wait for the host: the warp stops in front of
// ALLOC, the host does it once, every lane's write head moves into the new
// block keeping its offset, and the lanes go on next tick.
void test_host_instruction() {
    WFix f;
    CHECK(f.init("ALLOC INC NOP0 NOP0", 4, 10, "", 1, 100, 0));
    for (u32 l = 0; l < 4; ++l)
        f.lane(l).cx = 16; // AX, BX 0: memory
    f.s.tick(0);
    CHECK(f.s.tally().warp_host == 1 && f.s.warp(f.w).steps == 0 && f.s.warp(f.w).wait == 0);
    const u32 k = f.s.body(f.b).latest;
    CHECK(k != kNone && f.s.block(k).len == 16);
    const u32 at = f.s.block(k).start;
    for (u32 l = 0; l < 4; ++l)
        CHECK(f.lane(l).ip == 101 && f.lane(l).wh == at + l && f.lane(l).err == 0 && f.lane(l).cx == 16);
    f.s.tick(1);
    CHECK(f.lane(0).cx == 17); // ran on: INC
    CHECK(soup_ok(f.s));
}

// Block ops fail in a warp (the warp is the GPU); NPU ops and launches don't
// run on lanes either: they are the host's.
void test_block_ops_fail() {
    WFix f;
    CHECK(f.init("GPU NOP0 NOP0 NOP0", 2, 5));
    f.lane(0).cx = f.lane(1).cx = 4; // BX 0: a GPU copy
    f.s.tick(0);
    CHECK(f.s.tally().warp_host == 1 && f.lane(0).err == 1 && f.lane(1).err == 1 && f.lane(0).ip == 101);
}

// Launch (GPU verb, BX 20) from a CPU: a warp at the code after the matching
// template, every lane with the CPU's registers and heads, offset by lane.
void test_launch() {
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes = 4\nwarp.max = 2\n");
    SoupParams p;
    CHECK(make_params(&p, cfg.c_str(), 1024));
    Soup s;
    s.init(&p, 5, 0);
    const u32 b = plant(s, 100, "GPU NOP1 INC NOP0 INC INC", 500);
    CHECK(b != kNone && s.claim_gpus(b, 4) == 4);
    s.begin_tick(0);
    s.prepare_gpu();
    const u32 c = s.body(b).cpus;
    Cpu& cpu = s.cpu_mut(c);
    cpu.ax = 7;
    cpu.bx = kLaunchOp;
    cpu.cx = 9;
    cpu.rh = 100;
    cpu.wh = 103;
    s.run_cpu(c, 1);
    CHECK(s.cpu(c).err == 0 && s.cpu(c).ip == 102 && s.warp_count(b) == 1);
    const u32 w = s.warp_head(b);
    for (u32 l = 0; l < 4; ++l) {
        const Lane& x = s.lane(w, l);
        CHECK(x.ip == 104 && x.ax == 7 && x.bx == kLaunchOp && x.cx == 9 && x.rh == 100 + l &&
              x.wh == 100 + (3 + l) % 6);
    }
    CHECK(s.tally().launches == 1 && s.body(b).form == kFormCpu);
    // warp.max: a second launch works, a third fails
    s.cpu_mut(c).ip = 100;
    s.run_cpu(c, 1);
    s.cpu_mut(c).ip = 100;
    s.run_cpu(c, 1);
    CHECK(s.warp_count(b) == 2 && s.cpu(c).err == 1);
    CHECK(soup_ok(s));
}

// The two forms of birth: what a body is born on, with the fallback to the
// other form when it holds only that one's units.
void test_forms() {
    WFix f;
    CHECK(f.init("NOP0 NOP0", 2, 4));
    CHECK(f.s.body(f.b).form == kFormGpu && f.s.warp_count(f.b) == 1);
    // GPU wanted, no GPU units, millicores: born on a CPU
    Array<u8> code;
    TextBuf err;
    CHECK(genome_from_names(f.p.table, "NOP0 NOP0", &code, &err));
    for (u32 i = 0; i < 2; ++i)
        f.s.mem_mut()[400 + i] = f.s.mem_mut()[500 + i] = code[i];
    const u32 b1 = f.s.create_body_form(400, 2, 50, 0, kFormGpu);
    CHECK(b1 != kNone && f.s.body(b1).form == kFormCpu && f.s.body(b1).ncpu == 1 && f.s.warp_count(b1) == 0);
    // CPU wanted, no millicores, GPU units: born on the GPU
    const u32 b2 = f.s.create_body_form(500, 2, 0, 3, kFormCpu);
    CHECK(b2 != kNone && f.s.body(b2).form == kFormGpu && f.s.body(b2).ncpu == 0 && f.s.warp_count(b2) == 1);
    CHECK(f.s.tally().gpu_births == 2); // the fixture's body and b2
    CHECK(soup_ok(f.s));
}

// Sparks on a node without free CPU: with spark.gpu_grant they are born on
// the GPU.
void test_gpu_sparks() {
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 10\nworld.gpus = 400\ngpu.warps = true\nspark.grant = 10\n"
               "spark.gpu_grant = 4\nspark.initial = 30\nspark.gpu_born = 0\n");
    SoupParams p;
    CHECK(make_params(&p, cfg.c_str(), 4096));
    Soup s;
    s.init(&p, 5, 0);
    u32 cpu = 0, gpu = 0;
    for (usize k = 0; k < s.body_count(); ++k)
        (s.body(s.body_at(k)).form == kFormGpu ? gpu : cpu) += 1;
    CHECK(cpu == 1 && gpu == 29); // the first takes all 10 millicores; the rest only have GPU units
    CHECK(soup_ok(s));
}

// The death rule: no running CPU and no running warp. A warp ends when all
// its lanes halt; a body whose GPU units are gone can't run its warps.
void test_death_rule() {
    WFix f;
    CHECK(f.init("DIV DIV DIV", 2, 20, "cpu.halt_after = 3\n"));
    // DIV by zero fails every time: 3 errors halt each lane, the warp ends
    // and its body, with no CPU, dies
    f.s.tick(0);
    CHECK(!f.s.body(f.b).alive && f.s.warps_alive() == 0 && f.s.tally().deaths == 1);
    CHECK(soup_ok(f.s));
    // RELEASE of its one GPU unit (a host instruction): its warp can't run,
    // so it dies too
    WFix g;
    CHECK(g.init("RELEASE NOP0", 2, 4));
    for (u32 l = 0; l < 2; ++l) {
        g.lane(l).bx = 2; // GPU units
        g.lane(l).cx = 1;
    }
    g.s.tick(0);
    CHECK(!g.s.body(g.b).alive && g.s.free_gpus() == 64);
    CHECK(soup_ok(g.s));
}

// Costs: a step costs warp.step_cost plus warp.lane_cost a lane, the quota is
// the GPU units' capacity, and the last step's overshoot is a debt.
void test_costs() {
    WFix f;
    CHECK(f.init("INC INC INC INC", 4, 100, "warp.step_cost = 16\nwarp.lane_cost = 1\n"));
    f.s.tick(0); // 100 work, 20 a step: 5 steps exactly
    CHECK(f.s.warp(f.w).steps == 5 && f.s.warp(f.w).lane_instr == 20);
    WFix g;
    CHECK(g.init("INC INC INC INC", 4, 90, "warp.step_cost = 16\nwarp.lane_cost = 1\n"));
    g.s.tick(0); // 90: 5 steps (the 5th overshoots by 10)
    CHECK(g.s.warp(g.w).steps == 5 && g.s.warp_carry(g.b) < -9.9f && g.s.warp_carry(g.b) > -10.1f);
    g.s.tick(1); // 90 - 10 = 80: 4 steps
    CHECK(g.s.warp(g.w).steps == 9);
    // Memory upkeep: a body on the GPU alone pays it from its warps, at the
    // serial rate (a one-lane step an instruction: 17 work).
    WFix u;
    CHECK(u.init("INC INC INC INC", 4, 1000, "warp.step_cost = 16\nwarp.lane_cost = 1\nmem.upkeep = 2\n"
                                              "mem.upkeep_exp = 1\n"));
    u.s.tick(0); // upkeep 2 x 4 codons = 8 instructions = 136 work; 864 left: 43 steps, 4 over
    CHECK(u.s.warp(u.w).steps == 44 && u.s.tally().upkeep == 8);
}

// ENGULF takes the prey's warps along with its CPUs.
void test_engulf_takes_warps() {
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes = 2\n");
    SoupParams p;
    CHECK(make_params(&p, cfg.c_str(), 1024));
    Soup s;
    s.init(&p, 5, 0);
    const u32 eater = plant(s, 100, "ENGULF", 500);
    Array<u8> code;
    TextBuf err;
    CHECK(genome_from_names(p.table, "INC INC", &code, &err));
    s.mem_mut()[150] = code[0];
    s.mem_mut()[151] = code[1];
    const u32 prey = s.create_body_form(150, 2, 0, 4, kFormGpu);
    CHECK(eater != kNone && prey != kNone && s.warp_count(prey) == 1);
    s.begin_tick(0);
    s.prepare_gpu();
    s.run_cpu(s.body(eater).cpus, 1);
    CHECK(!s.body(prey).alive && s.warp_count(eater) == 1 && s.warp_count(prey) == 0);
    CHECK(s.warp(s.warp_head(eater)).body == eater && s.gpu_count(eater) == 4);
}

// The GPU-born replicator: on nodes with no CPU at all it fills the soup,
// its lineage meets the gate, and gpu_native fires. A snapshot taken midway
// resumes exactly, and the hash is the same at 1, 4 and 20 threads.
const char* warp_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append("seed = 7\nworld.model = soup\nworld.nodes = 4\nworld.soup_size = 4096\nworld.millicores = 400\n"
             "world.gpus = 400\ngpu.warps = true\ncpu.instr_per_core = 2500\nspark.initial = 0\n"
             "spark.rate = 0\nseed.at = 1000\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 64\n"
             "seed.genome = ");
    t.append(kWarpReplicator);
    t.append_char('\n');
    return t.c_str();
}

void test_gpu_replicator() {
    Run run;
    CHECK(make_run(run, warp_world(), 4));
    run.run_until(90);
    u32 gpu_bodies = 0, bodies = 0;
    for (u32 n = 0; n < run.world().node_count(); ++n) {
        const Soup& s = run.world().node_soup(n);
        bodies += static_cast<u32>(s.body_count());
        for (usize k = 0; k < s.body_count(); ++k)
            gpu_bodies += s.body(s.body_at(k)).form == kFormGpu;
        CHECK(soup_ok(s));
    }
    printf("  warp replicator: 90 ticks, %u bodies, %u born on a GPU; gpu_native %s at tick %llu\n", bodies,
           gpu_bodies, run.gpu_native().found ? "found" : "NOT found",
           static_cast<unsigned long long>(run.gpu_native().tick));
    CHECK(bodies > 40 && gpu_bodies == bodies);
    CHECK(run.detection().found && run.gpu_detection().found && run.gpu_native().found);
}

// The viewer (C14): a frame shows a node's running lanes, and a GPU-born
// body's form, warps and lanes (those of the warp's next step marked).
void test_view_warps() {
    TextBuf cfg;
    cfg.append(warp_world());
    cfg.append("warp.device = cpu\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 2));
    ViewSession vs;
    vs.attach(&run, 10);
    for (u32 t = 0; t < 12; ++t) {
        run.step();
        vs.after_step();
    }
    const Soup& s = run.world().node_soup(0);
    u32 b = kNone;
    for (usize k = 0; k < s.body_count() && b == kNone; ++k)
        if (s.warp_count(s.body_at(k)) > 0)
            b = s.body_at(k);
    CHECK(b != kNone);
    ViewRequest q;
    q.node = 0;
    q.body = b;
    q.serial = s.body(b).serial;
    ViewFrame f;
    vs.build(q, &f);
    CHECK(f.body_ok && f.body.form == kFormGpu && f.body.warps == s.warp_count(b));
    CHECK(f.lanes.size() == static_cast<usize>(s.warp_count(b)) * s.lanes_per_warp());
    u32 next = 0;
    for (usize i = 0; i < f.lanes.size(); ++i)
        next += f.lanes[i].next;
    CHECK(next > 0 && f.node[0].lanes > 0 && f.node[0].gpu_bodies == s.body_count());
    CHECK(node_metric(f.node[0], kColorLanes) == f.node[0].lanes && strcmp(color_by_name(kColorLanes), "GPU lanes") == 0);
}

// A busy world with every path of the warp code in it: a procedural world
// with dispersal, random sparks of both forms (random code on lanes does
// host instructions, launches, ENGULFs ...), the GPU-born replicator on every
// node, heat, hazards, salvage and the disk.
const char* busy_warp_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append("seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 16\nworld.soup_size = 2048\n"
             "world.millicores = 300\nworld.gpus = 300\ngpu.warps = true\n"
             "edge.bandwidth = 8\ncpu.instr_per_core = 2500\ncpu.wear = 0.002\ngpu.wear = 0.002\n"
             "spark.timing = last\nspark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.gpu_grant = 6\n"
             "spark.min_len = 8\nspark.max_len = 32\nhazard.copy = 0.005\nhazard.flaw = 0.001\n"
             "hazard.rot = 0.00005\nsalvage.ratio = 4\nworld.disk = 512\nheat.gpu_gain = 30\n"
             "seed.at = 600\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 64\nseed.genome = ");
    t.append(kWarpReplicator);
    t.append_char('\n');
    return t.c_str();
}

void test_determinism() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, busy_warp_world(), threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            Rng r(99, 0, 0);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            run.set_dispatch_order(order.data());
        }
        run.run_until(200);
        hashes[v] = run.world().hash();
        if (v == 0) {
            u64 warps = 0, gpu = 0, steps = 0;
            for (u32 n = 0; n < run.world().node_count(); ++n) {
                const Soup& s = run.world().node_soup(n);
                warps += s.warps_alive();
                for (usize k = 0; k < s.body_count(); ++k)
                    gpu += s.body(s.body_at(k)).form == kFormGpu;
                CHECK(soup_ok(s));
            }
            for (u32 n = 0; n < run.world().node_count(); ++n)
                for (usize w = 0; w < run.world().node_soup(n).warp_slots(); ++w)
                    steps += run.world().node_soup(n).warp(static_cast<u32>(w)).steps;
            printf("  busy warp world: 200 ticks, %llu warps, %llu GPU-born bodies, hash %016llx\n",
                   static_cast<unsigned long long>(warps), static_cast<unsigned long long>(gpu),
                   static_cast<unsigned long long>(hashes[0]));
            CHECK(warps > 0 && gpu > 0);
        }
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);

    Run a;
    CHECK(make_run(a, busy_warp_world(), 4));
    a.run_until(100);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(200);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
}

// The real GPU (8b): the busy world with warps on the device gives the
// reference's hash; with warp.verify = 1 every round is also run on the
// reference and compared, word for word.
void test_gpu_device() {
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < n; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    if (!gpu) {
        printf("  warp: no hardware GPU, device tests skipped\n");
        return;
    }
    u64 want = 0;
    {
        TextBuf cfg;
        cfg.append(busy_warp_world());
        cfg.append("warp.device = cpu\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        run.run_until(120);
        want = run.world().hash();
        CHECK(run.world().warp_gpu_rounds() == 0 && strcmp(run.world().warp_device_name(), "cpu") == 0);
    }
    TextBuf cfg;
    cfg.append(busy_warp_world());
    cfg.append("warp.device = gpu\nwarp.verify = 1\n");
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    run.run_until(120);
    const World& w = run.world();
    printf("  warp GPU: %s, kernel compiled in %.0f ms; %llu rounds on the GPU, %llu verified, %llu differed\n",
           w.warp_device_name(), w.warp_compile_ms(), static_cast<unsigned long long>(w.warp_gpu_rounds()),
           static_cast<unsigned long long>(w.warp_verified()), static_cast<unsigned long long>(w.warp_mismatches()));
    if (w.warp_notes().n)
        printf("  warp notes: %.*s", static_cast<int>(w.warp_notes().n), w.warp_notes().p);
    CHECK(w.warp_gpu_rounds() >= 120 && w.warp_ref_rounds() == 0);
    CHECK(w.warp_verified() == w.warp_gpu_rounds() && w.warp_mismatches() == 0);
    CHECK_EQ_U64(w.hash(), want);
    CHECK(w.warp_gpu_stats().lanes_fetched > 0); // host instructions brought lanes back

    // The GPU keeps lanes between rounds. warp.device = mixed splits the
    // nodes between the GPU and the CPU at random every round, so lanes
    // cross both ways all the time, through 20 threads, a snapshot and a
    // check of every third GPU round: still the reference's world.
    TextBuf mc;
    mc.append(busy_warp_world());
    mc.append("warp.device = mixed\nwarp.verify = 3\n");
    Run mixed;
    CHECK(make_run(mixed, mc.c_str(), 20));
    mixed.run_until(60);
    ByteWriter snap;
    mixed.snapshot(snap);
    mixed.run_until(120);
    const World& mw = mixed.world();
    printf("  warp mixed: %llu node-rounds on the GPU, %llu on the CPU, %llu warps' lanes sent and %llu fetched\n",
           static_cast<unsigned long long>(mw.warp_gpu_nodes()), static_cast<unsigned long long>(mw.warp_ref_nodes()),
           static_cast<unsigned long long>(mw.warp_gpu_stats().lanes_sent),
           static_cast<unsigned long long>(mw.warp_gpu_stats().lanes_fetched));
    CHECK(mw.warp_gpu_nodes() > 0 && mw.warp_ref_nodes() > 0 && mw.warp_mismatches() == 0 && !mw.warp_failed());
    CHECK_EQ_U64(mw.hash(), want);
    Run resumed;
    TextBuf err;
    CHECK(resumed.init_from_snapshot(snap.data(), snap.size(), 4, &err));
    resumed.run_until(120);
    CHECK_EQ_U64(resumed.world().hash(), want);
    // auto: the split moves with the measured times, and whole ticks go with
    // or without the GPU (the first 10 alternate); the same world
    TextBuf ac;
    ac.append(busy_warp_world());
    ac.append("warp.device = auto\n");
    Run au;
    CHECK(make_run(au, ac.c_str(), 4));
    au.run_until(120);
    CHECK(au.world().warp_plain_ticks() >= 5 && au.world().warp_split_ticks() >= 5);
    CHECK_EQ_U64(au.world().hash(), want);
}

// gpu.warps = false is Phase 7 exactly: the same hash as no key at all.
void test_off_is_phase7() {
    TextBuf off;
    off.append(kSoupWorldProc);
    off.append("gpu.warps = false\nspark.gpu_grant = 5\n");
    Run a, b;
    CHECK(make_run(a, kSoupWorldProc, 4));
    CHECK(make_run(b, off.c_str(), 4));
    a.run_until(60);
    b.run_until(60);
    CHECK_EQ_U64(b.world().hash(), a.world().hash());
    // seed.form = gpu needs warps
    TextBuf bad;
    bad.append(kSoupWorldProc);
    bad.append("seed.form = gpu\n");
    Run c;
    Config::ParseError pe;
    CHECK(c.config().parse(bad.view(), &pe));
    TextBuf err;
    CHECK(!c.init(1, &err));
}

} // namespace

void suite_warp() {
    test_lockstep();
    test_divergence();
    test_offset_copy();
    test_read_then_write();
    test_own_body();
    test_host_instruction();
    test_block_ops_fail();
    test_launch();
    test_forms();
    test_gpu_sparks();
    test_death_rule();
    test_costs();
    test_engulf_takes_warps();
    test_gpu_replicator();
    test_view_warps();
    test_determinism();
    test_gpu_device();
    test_off_is_phase7();
}
