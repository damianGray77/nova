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
        cfg.append("warp.lanes_min = ");
        cfg.append_u64(lanes);
        cfg.append("\nwarp.lanes_max = ");
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
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 4\nwarp.lanes_max = 4\nwarp.max = 2\n");
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

// docs/lanes-plan.md L1, L2 (Damian, 2026-11-09): each warp its own lanes,
// on a node of 256. A body born on the GPU gets the fewest that cover its
// code (16 at least, the node's width at most); a launch asks for CX lanes.
// Only its lanes run (the rest of its slot is halted), heads move by them and
// SENSE 21 reads them.
void test_own_lanes() {
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 16\nwarp.lanes_max = 256\n"
               "warp.step_cost = 1\nwarp.lane_cost = 1\nwarp.capacity = 10\nwarp.max = 4\n");
    SoupParams p;
    CHECK(make_params(&p, cfg.c_str(), 4096));
    set_warp_lanes(&p, 256); // a node of 256 lanes, as World::derive_node draws one
    Soup s;
    s.init(&p, 5, 0);
    u32 v21 = 0; // SENSE's BX for reading 21, the lanes in my warp
    while (v21 < p.sel.sense_n && p.sel.sense[v21] != 21)
        ++v21;
    CHECK(v21 < p.sel.sense_n);
    auto gpu_body = [&](u32 at, u32 len, const char* first) {
        TextBuf prog;
        prog.append(first);
        for (u32 i = 1; i < len; ++i)
            prog.append(" NOP0");
        Array<u8> code;
        TextBuf err;
        if (!genome_from_names(p.table, prog.c_str(), &code, &err))
            return kNone;
        for (usize i = 0; i < code.size(); ++i)
            s.mem_mut()[at + i] = code[i];
        return s.create_body_form(at, len, 0, 1, kFormGpu);
    };
    const u32 a = gpu_body(100, 20, "SENSE"), b = gpu_body(200, 3, "NOP0"), c = gpu_body(300, 100, "NOP0"),
              d = gpu_body(500, 300, "NOP0");
    CHECK(a != kNone && b != kNone && c != kNone && d != kNone);
    const u32 wa = s.warp_head(a);
    CHECK(s.warp_lanes(wa) == 32 && s.warp_lanes(s.warp_head(b)) == 16 && s.warp_lanes(s.warp_head(c)) == 128 &&
          s.warp_lanes(s.warp_head(d)) == 256);
    CHECK(s.lanes_per_warp() == 256 && s.warp_live(wa) == 32);
    for (u32 l = 0; l < 256; ++l)
        s.lane_mut(wa, l).bx = v21;
    s.tick(0); // one step each (1 + 1 a lane > 10): SENSE 21 on its 32 lanes, the rest never run
    CHECK(s.warp(wa).steps == 1 && s.warp(wa).lane_instr == 32);
    bool own = true, idle = true;
    for (u32 l = 0; l < 32; ++l)
        own = own && s.lane(wa, l).cx == 32 && s.lane(wa, l).ip == 101 && s.lane(wa, l).rh == 100 + l % 20;
    for (u32 l = 32; l < 256; ++l)
        idle = idle && s.lane(wa, l).halted && s.lane(wa, l).cx == 0 && s.lane(wa, l).ip == 0;
    CHECK(own && idle && s.tally().halts == 0);
    CHECK(soup_ok(s));
    // A launch asks for CX lanes: 40 -> 64, 0 -> 16, 5,000 -> the node's 256.
    const u32 cb = plant(s, 1000, "GPU NOP1 INC NOP0 INC INC", 500);
    CHECK(cb != kNone && s.claim_gpus(cb, 4) == 4);
    s.begin_tick(1);
    s.prepare_gpu();
    const u32 cc = s.body(cb).cpus;
    const u32 asks[3] = {40, 0, 5000}, want[3] = {64, 16, 256};
    u32 w = kNone;
    for (u32 i = 0; i < 3; ++i) {
        Cpu& cpu = s.cpu_mut(cc);
        cpu.ip = 1000;
        cpu.bx = kLaunchOp;
        cpu.cx = asks[i];
        s.run_cpu(cc, 1);
        CHECK(s.cpu(cc).err == 0);
        w = s.warp_head(cb);
        while (s.warp(w).next != kNone)
            w = s.warp(w).next;
        CHECK(s.warp_lanes(w) == want[i] && s.warp_live(w) == want[i]);
    }
    CHECK(s.warp_count(cb) == 3 && s.lane(w, 255).cx == 5000 && s.lane(w, 255).rh == 1000 + 255 % 6);
    CHECK(soup_ok(s));
}

// L2: a warp's daughter is born with its lanes, not fit to her code: a CPU
// launches a 128-lane warp (CX 100) that ALLOCs 16 codons and DIVIDEs them
// off with half its holdings; she is GPU-born with 128 lanes.
void test_lanes_inherited() {
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 16\nwarp.lanes_max = 256\n"
               "warp.step_cost = 1\nwarp.lane_cost = 0\nwarp.capacity = 1000\n");
    SoupParams p;
    CHECK(make_params(&p, cfg.c_str(), 4096));
    set_warp_lanes(&p, 256);
    Soup s;
    s.init(&p, 5, 0);
    // The warp's code, after NOP0: AX <-> BX (BX 0: memory, here), CX = 16,
    // ALLOC; CX = 32, DIVIDE (32/64 of its holdings).
    const u32 b = plant(s, 100, "GPU NOP1 INC NOP0 SWAP ZERO INC SHL SHL SHL SHL ALLOC ZERO INC SHL SHL SHL SHL SHL "
                        "DIVIDE", 500);
    CHECK(b != kNone && s.claim_gpus(b, 8) == 8);
    s.begin_tick(0);
    s.prepare_gpu();
    const u32 c = s.body(b).cpus;
    Cpu& cpu = s.cpu_mut(c);
    cpu.ax = 0;
    cpu.bx = kLaunchOp;
    cpu.cx = 100;
    s.run_cpu(c, 1);
    CHECK(s.warp_count(b) == 1 && s.warp_lanes(s.warp_head(b)) == 128);
    s.cpu_mut(c).halted = 1; // (the CPU stops: only the warp runs on)
    u32 d = kNone;
    for (u64 t = 1; t < 20 && d == kNone; ++t) {
        s.tick(t);
        for (usize k = 0; k < s.body_count(); ++k)
            if (s.body_at(k) != b && s.body(s.body_at(k)).form == kFormGpu)
                d = s.body_at(k);
    }
    CHECK(d != kNone);
    if (d == kNone)
        return;
    CHECK(s.block(s.body(d).blocks).len == 16 && s.warp_count(d) == 1);
    CHECK(s.warp_lanes(s.warp_head(d)) == 128 && s.tally().births == 1); // (fit to her code she'd have 16)
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
    cfg.append("world.millicores = 1000\nworld.gpus = 64\ngpu.warps = true\nwarp.lanes_min = 2\nwarp.lanes_max = 2\n");
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

// Warp widths by node (docs/inject-plan.md W1-W3; Damian: up to 1,024): the
// busy world with every node drawing its width from 16 to 1,024 lanes. The
// widths are the world seed's, powers of two, several of them; the world is
// the same at 1 and 20 threads and through a snapshot; and on the real GPU,
// each width on its own kernel, every round checked against the reference,
// the same world again (warp.device = mixed too, lanes crossing both ways).
void test_mixed_widths() {
    TextBuf base;
    base.append(busy_warp_world());
    base.append("warp.lanes_min = 16\nwarp.lanes_max = 1024\n");
    const u64 ticks = 80;
    u64 want = 0;
    u32 seen = 0; // widths seen, a bit each
    {
        TextBuf cfg;
        cfg.append(base.view());
        cfg.append("warp.device = cpu\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        const World& w = run.world();
        bool pow2 = true;
        for (u32 n = 0; n < w.node_count(); ++n) {
            const u32 l = w.node_soup(n).lanes_per_warp();
            pow2 = pow2 && l >= 16 && l <= 1024 && (l & (l - 1)) == 0;
            seen |= l;
        }
        u32 kinds = 0;
        for (u32 b = seen; b; b &= b - 1)
            ++kinds;
        CHECK(pow2 && kinds >= 4);
        run.run_until(ticks);
        want = run.world().hash();
        u64 warps = 0;
        for (u32 n = 0; n < w.node_count(); ++n)
            warps += w.node_soup(n).warps_alive();
        CHECK(warps > 0);
        printf("  mixed widths: 16 nodes, %u widths from 16 to 1,024 lanes; %llu ticks, %llu warps live, hash %016llx\n",
               kinds, static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(warps),
               static_cast<unsigned long long>(want));
    }
    {
        TextBuf cfg;
        cfg.append(base.view());
        cfg.append("warp.device = cpu\n");
        Run one, many;
        CHECK(make_run(one, cfg.c_str(), 1) && make_run(many, cfg.c_str(), 20));
        one.run_until(ticks / 2);
        ByteWriter snap;
        one.snapshot(snap);
        one.run_until(ticks);
        many.run_until(ticks);
        CHECK_EQ_U64(one.world().hash(), want);
        CHECK_EQ_U64(many.world().hash(), want);
        Run back;
        TextBuf err;
        CHECK(back.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        back.run_until(ticks);
        CHECK_EQ_U64(back.world().hash(), want);
    }
    AccelDevice devs[16];
    const u32 nd = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < nd; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    if (!gpu) {
        printf("  mixed widths: no hardware GPU, the GPU's runs skipped\n");
        return;
    }
    const char* const devices[2] = {"gpu", "mixed"};
    for (u32 d = 0; d < 2; ++d) {
        TextBuf cfg;
        cfg.append(base.view());
        cfg.append("warp.device = ");
        cfg.append(devices[d]);
        cfg.append("\nwarp.verify = 1\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), d == 0 ? 4 : 20));
        run.run_until(ticks);
        const World& w = run.world();
        printf("  mixed widths, warp.device = %s: %llu rounds on the GPU, %llu verified, %llu differed (%s)\n",
               devices[d], static_cast<unsigned long long>(w.warp_gpu_rounds()),
               static_cast<unsigned long long>(w.warp_verified()), static_cast<unsigned long long>(w.warp_mismatches()),
               w.warp_kernel());
        if (w.warp_notes().n && w.warp_gpu_rounds() == 0)
            printf("  warp notes: %.*s", static_cast<int>(w.warp_notes().n), w.warp_notes().p);
        CHECK(w.warp_gpu_rounds() > 0 && w.warp_verified() == w.warp_gpu_rounds() && w.warp_mismatches() == 0);
        CHECK_EQ_U64(w.hash(), want);
    }
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
    // 32 lanes run DXC's kernel when the build has it (docs/dxc-plan.md D3);
    // warp.kernel = fxc runs FXC's, with every round verified too.
    printf("  warp kernel: %s\n", w.warp_kernel());
    if (warp_dxc_built())
        CHECK(strncmp(w.warp_kernel(), "DXC", 3) == 0);
    {
        TextBuf fc;
        fc.append(busy_warp_world());
        fc.append("warp.device = gpu\nwarp.verify = 1\nwarp.kernel = fxc\n");
        Run fr;
        CHECK(make_run(fr, fc.c_str(), 4));
        fr.run_until(120);
        const World& fw = fr.world();
        printf("  warp kernel: %s\n", fw.warp_kernel());
        CHECK(strncmp(fw.warp_kernel(), "FXC", 3) == 0);
        CHECK(fw.warp_gpu_rounds() >= 120 && fw.warp_verified() == fw.warp_gpu_rounds() && fw.warp_mismatches() == 0);
        CHECK_EQ_U64(fw.hash(), want);
    }

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
    printf("  warp mixed: %llu body-rounds on the GPU, %llu on the CPU, %llu warps' lanes sent and %llu fetched\n",
           static_cast<unsigned long long>(mw.warp_gpu_bodies()), static_cast<unsigned long long>(mw.warp_ref_bodies()),
           static_cast<unsigned long long>(mw.warp_gpu_stats().lanes_sent),
           static_cast<unsigned long long>(mw.warp_gpu_stats().lanes_fetched));
    CHECK(mw.warp_gpu_bodies() > 0 && mw.warp_ref_bodies() > 0 && mw.warp_mismatches() == 0 && !mw.warp_failed());
    CHECK_EQ_U64(mw.hash(), want);
    Run resumed;
    TextBuf err;
    CHECK(resumed.init_from_snapshot(snap.data(), snap.size(), 4, &err));
    resumed.run_until(120);
    CHECK_EQ_U64(resumed.world().hash(), want);
    // auto: the split moves with the measured times, and whole ticks go with
    // or without the GPU (five of each first); the same world
    TextBuf ac;
    ac.append(busy_warp_world());
    ac.append("warp.device = auto\n");
    Run au;
    CHECK(make_run(au, ac.c_str(), 4));
    au.run_until(120);
    CHECK(au.world().warp_plain_ticks() >= 5 && au.world().warp_split_ticks() >= 5);
    CHECK_EQ_U64(au.world().hash(), want);
    // The overlap plan: warp.groups = 2 runs a tick's round in two node groups
    // on the GPU's two batches, staggered (World::warp_round0_groups). Only
    // when its phases run changes: the same world on the GPU, mixed (lanes
    // crossing every round) and auto, at 1, 20 and 4 threads, and through a
    // snapshot taken mid-run.
    const char* const grouped[3] = {"warp.device = gpu\nwarp.groups = 2\n", "warp.device = mixed\nwarp.groups = 2\n",
                                    "warp.device = auto\nwarp.groups = 2\n"};
    const u32 gthreads[3] = {1, 20, 4};
    for (u32 i = 0; i < 3; ++i) {
        TextBuf gc;
        gc.append(busy_warp_world());
        gc.append(grouped[i]);
        Run gr;
        CHECK(make_run(gr, gc.c_str(), gthreads[i]));
        gr.run_until(60);
        ByteWriter gs;
        gr.snapshot(gs);
        gr.run_until(120);
        const World& gw = gr.world();
        if (i < 2) // (auto groups only once its model has its rates, which this small world may not reach)
            CHECK(gw.warp_grouped_ticks() >= 100);
        CHECK(!gw.warp_failed() && gw.warp_mismatches() == 0);
        CHECK_EQ_U64(gw.hash(), want);
        Run gres;
        CHECK(gres.init_from_snapshot(gs.data(), gs.size(), 4, &err));
        gres.run_until(120);
        CHECK_EQ_U64(gres.world().hash(), want);
    }
}

// A loop of signed divisions, from a warp that differed on the GPU in Phase 10
// profiling (warp.verify, node 577 at tick 35725): run N steps on the GPU and
// on the reference, for every N, and the lanes must agree. With the GPU's own
// integer division, 1593955456 / 33913946 came out 173 (not 46) at step 164
// (warp_isa.inc, w_ulong); each lane divides a different dividend.
void test_gpu_div_loop() {
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 dev = kNone;
    for (u32 i = 0; i < n && dev == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            dev = i;
    if (dev == kNone)
        return;
    for (u32 kind = 0; kind < 2; ++kind) { // DXC's kernel (where the build has it), then FXC's
        WarpGpu gpu;
        TextBuf err;
        WarpKernelOptions kopt;
        kopt.fxc = kind == 1;
        CHECK(gpu.open(dev, WarpGpu::width_bit(32), &err, nullptr, &kopt));
        if (!gpu.is_open())
            return;
        const char* prog = "DIV INC ROT NOP1 IFZ NOP0 DIV SWAP";
        auto start = [](WFix& f) {
            for (u32 l = 0; l < 32; ++l) {
                Lane& x = f.lane(l);
                x.ip = 106; // the second DIV
                x.ax = 0xffffffbbu;
                x.bx = 0xa0fe2b80u + l; // a different dividend per lane
                x.cx = 0x01568f9du;
            }
        };
        u32 first_bad = 0;
        for (u32 steps = 1; steps <= 490 && !first_bad; ++steps) {
            WFix r, g;
            CHECK(r.init(prog, 32, steps));
            CHECK(g.init(prog, 32, steps));
            start(r);
            start(g);
            r.s.tick(0); // the reference
            g.s.tick_first(0);
            CHECK(g.s.warp_pack(0));
            Array<u32> send;
            send.push(g.w);
            WarpJob j = {};
            j.node = 0;
            j.img = g.s.warp_image().data();
            j.img_words = static_cast<u32>(g.s.warp_image().size());
            j.recs = g.s.warp_records().data();
            j.nrec = static_cast<u32>(g.s.warp_records().size());
            j.lanes = g.s.lanes_mut().data();
            j.nlanes = static_cast<u32>(g.s.lanes_mut().size());
            j.wl = g.s.lanes_per_warp();
            j.send = send.data();
            j.nsend = 1;
            CHECK(gpu.run(g.s.warp_consts().data(), static_cast<u32>(g.s.warp_consts().size()), &j, 1, &err));
            WarpFetch f = {0, g.w, g.s.lanes_mut().data() + static_cast<usize>(g.w) * 32};
            CHECK(gpu.fetch(&f, 1, &err));
            for (u32 l = 0; l < 32 && !first_bad; ++l) {
                const Lane& a = r.lane(l);
                const Lane& b = g.lane(l);
                if (memcmp(&a, &b, sizeof(Lane)) != 0) {
                    first_bad = steps;
                    printf("  div loop: lane %u differs after %u steps: reference ip %u ax %08x bx %08x cx %08x, GPU ip %u "
                           "ax %08x bx %08x cx %08x\n",
                           l, steps, a.ip, a.ax, a.bx, a.cx, b.ip, b.ax, b.bx, b.cx);
                }
            }
        }
        CHECK(first_bad == 0);
        printf("  warp GPU division (%s): a loop of DIVs agrees with the reference for 1 to 490 steps\n",
               gpu.kernel());
    }
}

// The world reads a round where it came back (WarpGpu::finish with copy_back
// false, 2026-11-08): result() holds exactly the words a copied-back round
// leaves in the image, the job's own image stays as it was sent, and a soup
// unpacked from result() ends the tick as one unpacked from the copy.
void test_gpu_results_in_place() {
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 dev = kNone;
    for (u32 i = 0; i < n && dev == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            dev = i;
    if (dev == kNone)
        return;
    WarpGpu gpu;
    TextBuf err;
    CHECK(gpu.open(dev, WarpGpu::width_bit(32), &err));
    if (!gpu.is_open())
        return;
    WFix f[2]; // the same body twice: one round copied back, one left in place
    WarpJob j[2] = {};
    Array<u32> send[2];
    for (u32 k = 0; k < 2; ++k) {
        CHECK(f[k].init("DIV INC ROT NOP1 IFZ NOP0 DIV SWAP", 32, 300));
        for (u32 l = 0; l < 32; ++l) {
            Lane& x = f[k].lane(l);
            x.ax = 7 + l;
            x.bx = 1000 * l + 3;
            x.cx = 11;
        }
        f[k].s.tick_first(0);
        CHECK(f[k].s.warp_pack(0));
        send[k].push(f[k].w);
        j[k].node = k; // (the GPU keeps each node's lanes apart)
        j[k].img = f[k].s.warp_image().data();
        j[k].img_words = static_cast<u32>(f[k].s.warp_image().size());
        j[k].recs = f[k].s.warp_records().data();
        j[k].nrec = static_cast<u32>(f[k].s.warp_records().size());
        j[k].lanes = f[k].s.lanes_mut().data();
        j[k].nlanes = static_cast<u32>(f[k].s.lanes_mut().size());
        j[k].wl = f[k].s.lanes_per_warp();
        j[k].send = send[k].data();
        j[k].nsend = 1;
    }
    Array<u32> as_sent;
    as_sent.assign(f[1].s.warp_image().data(), f[1].s.warp_image().size());
    const u32* cst = f[0].s.warp_consts().data();
    const u32 ncst = static_cast<u32>(f[0].s.warp_consts().size());
    CHECK(gpu.run(cst, ncst, &j[0], 1, &err)); // copied back into f[0]'s image
    CHECK(gpu.submit(cst, ncst, &j[1], 1, &err));
    CHECK(gpu.finish(&err, nullptr, 0, false)); // left where it came back
    const u32* res = gpu.result(0);
    CHECK(res != nullptr && gpu.result(1) == nullptr);
    const Array<u32>& copied = f[0].s.warp_image();
    CHECK(copied.size() == as_sent.size() && j[1].nrec == 1 && j[1].recs[0] == 0); // (one record: all of the image)
    CHECK(memcmp(res, copied.data(), copied.size() * 4) == 0);
    CHECK(memcmp(copied.data(), as_sent.data(), as_sent.size() * 4) != 0); // (the round changed it)
    CHECK(memcmp(f[1].s.warp_image().data(), as_sent.data(), as_sent.size() * 4) == 0); // not copied
    f[1].s.warp_set_result(res);
    for (u32 k = 0; k < 2; ++k) {
        f[k].s.warp_record_on_gpu().fill(1);
        f[k].s.warp_unpack();
        WarpFetch q = {k, f[k].w, f[k].s.lanes_mut().data() + static_cast<usize>(f[k].w) * 32};
        CHECK(gpu.fetch(&q, 1, &err));
        f[k].s.set_lane_state(f[k].w, Soup::kLanesBoth);
        f[k].s.warp_host(0);
        f[k].s.tick_second();
    }
    CHECK(f[0].s.tally().warp_steps > 0 && f[1].s.tally().warp_steps == f[0].s.tally().warp_steps);
    Hasher h0, h1;
    f[0].s.hash_into(h0);
    f[1].s.hash_into(h1);
    CHECK_EQ_U64(h1.finish(), h0.finish());
    printf("  warp GPU results in place: the same words as copied back, and the same soup after the tick\n");
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

// --- Turing completeness, shown ---------------------------------------------------
// Any two-symbol Turing machine compiles into Nova code (tm_compile): its tape
// is a stretch of the body's own memory (codon 0 a blank, codon 1 a mark), its
// head's address stays in AX, and each state is a block of code -- point the
// read head at the cell (SETH) and READ it, IFZ to the symbol's case, then
// point the write head there, WRITE, move AX a cell, and jump to the next
// state's label (a template, as every Nova jump). The halt state is a jump to
// itself. The same code runs on a CPU and on a warp: with S lanes, lane L's
// heads sit L codons past AX (SETH), so the tape is interleaved, cell i of
// lane L at i x S + L, and each lane runs its own copy of the machine.
// Reading, writing at a computed address, a conditional branch and an
// unbounded loop are all a Turing machine needs, so either form simulates any
// machine its memory holds.
struct TmRule {
    u8 write, right, next; // next: a state, or kTmHalt
};
constexpr u8 kTmHalt = 0xFF;
struct TuringMachine {
    u32 states;
    TmRule rule[4][2]; // [state][symbol read]
};

// Labels and their jump templates: 5 codons, a label starting NOP1 (so no
// template, no tape cell and no stretch of NOP0s is one), its template the
// complement. Ids: the tape 0 (the one label NOPs can follow -- in nova_v1 a
// blank is NOP0 -- so its shifted windows start NOP0 and are no label),
// states 1-4, each state's blank case 5-8, the halt 9.
void tm_bits(TextBuf& p, u32 id, bool label) {
    p.append(label ? " NOP1" : " NOP0");
    for (int k = 3; k >= 0; --k)
        p.append((((id >> k) & 1) != 0) == label ? " NOP1" : " NOP0");
}
// CX = k (ZERO, then a SHL and maybe an INC a bit, from the top bit).
void tm_const(TextBuf& p, u32 k) {
    p.append(" ZERO");
    int top = 31;
    while (top > 0 && !((k >> top) & 1))
        --top;
    for (int b = top; b >= 0; --b)
        p.append(((k >> b) & 1) ? " SHL INC" : " SHL");
}
// AX += k or AX -= k, keeping AX's other registers' roles: CX = k, then
// (h, B, k) -ROT-> (k, h, B) -SWAP-> (h, k, B) -ADD/SUB-> CX = h +- k -ROT-> (h +- k, h, k).
void tm_move(TextBuf& p, u32 k, bool right) {
    tm_const(p, k);
    p.append(right ? " ROT SWAP ADD ROT" : " ROT SWAP SUB ROT");
}

struct TmProgram {
    TextBuf names;  // the genome, as instruction names
    u32 tape = 0;   // where the tape's first cell is, from the genome's start
    u32 halt = 0;   // where the halt state's jump is
};

// The machine, the stride (lanes), tape cells, the cell the head starts on,
// and the name of codon 0 in the table (the blank).
TmProgram tm_compile(const TuringMachine& m, u32 stride, u32 cells, u32 start, const char* blank) {
    TmProgram out;
    TextBuf& p = out.names;
    u32 n = 0; // codons so far (every name is one)
    auto count = [&]() {
        n = 0;
        for (usize i = 0; i < p.size(); ++i)
            n += p.c_str()[i] == ' ';
    };
    p.append(" FIND");
    tm_bits(p, 0, false); // AX = the tape's first cell
    tm_move(p, start * stride, true);
    p.append(" JMP");
    tm_bits(p, 1, false);
    p.append(" ZERO"); // (never run: ends the template)
    for (u32 s = 0; s < m.states; ++s) {
        for (u32 sym = 2; sym-- > 0;) { // the mark's case falls through from IFZ, the blank's has its own label
            if (sym == 1) {
                tm_bits(p, 1 + s, true);
                p.append(" ZERO ROT SWAP SETH READ"); // read head at AX; CX = the cell
                p.append(" IFZ JMP");                // a blank: to its case; a mark: the JMP skipped, on
                tm_bits(p, 5 + s, false);
            } else {
                tm_bits(p, 5 + s, true);
            }
            const TmRule& r = m.rule[s][sym];
            p.append(" ZERO INC ROT SWAP SETH ZERO"); // write head at AX, CX = the symbol
            if (r.write)
                p.append(" INC");
            p.append(" WRITE");
            tm_move(p, stride, r.right != 0);
            p.append(" JMP");
            tm_bits(p, r.next == kTmHalt ? 9 : 1 + r.next, false);
            p.append(" ZERO");
        }
    }
    tm_bits(p, 9, true); // the halt: a jump to itself
    count();
    out.halt = n;
    p.append(" JMP");
    tm_bits(p, 9, false);
    p.append(" ZERO");
    tm_bits(p, 0, true); // the tape
    count();
    out.tape = n;
    for (u32 i = 0; i < cells * stride; ++i) {
        p.append_char(' ');
        p.append(blank);
    }
    return out;
}

// The machine on a C++ tape, the answer the soup must reach.
u32 tm_run(const TuringMachine& m, u32 cells, u32 start, u8* tape, u32* head_lo, u32* head_hi) {
    u32 s = 0, h = start, steps = 0;
    *head_lo = *head_hi = h;
    while (s != kTmHalt && steps < 1000000) {
        const TmRule& r = m.rule[s][tape[h]];
        tape[h] = r.write;
        h = r.right ? h + 1 : h - 1;
        s = r.next;
        ++steps;
        *head_lo = h < *head_lo ? h : *head_lo;
        *head_hi = h > *head_hi ? h : *head_hi;
        if (h >= cells)
            return 0; // (off the tape: the test's machine never is)
    }
    return steps;
}

void test_turing_machine() {
    // The 4-state busy beaver (Brady): 107 steps, 13 marks.
    const TuringMachine bb4 = {4,
                               {{{1, 1, 1}, {1, 0, 1}},            // A: 0 -> 1RB, 1 -> 1LB
                                {{1, 0, 0}, {0, 0, 2}},            // B: 0 -> 1LA, 1 -> 0LC
                                {{1, 1, kTmHalt}, {1, 0, 3}},      // C: 0 -> 1RH, 1 -> 1LD
                                {{1, 1, 3}, {0, 1, 0}}}};          // D: 0 -> 1RD, 1 -> 0RA
    // On a roomy tape first, for the head's range; then the tape the soup
    // gets, that range and a cell each side (a 32-lane warp's tape is 32
    // codons a cell, and a genome is at most 1,024).
    u8 roomy[64] = {};
    u32 lo = 0, hi = 0;
    const u32 steps = tm_run(bb4, 64, 32, roomy, &lo, &hi);
    const u32 cells = hi - lo + 3, start = 32 - lo + 1;
    u8 want[64] = {};
    u32 lo2 = 0, hi2 = 0;
    CHECK(cells <= 24 && tm_run(bb4, cells, start, want, &lo2, &hi2) == steps);
    u32 marks = 0;
    for (u32 i = 0; i < cells; ++i)
        marks += want[i];
    CHECK(steps == 107 && marks == 13 && lo2 > 0 && hi2 + 1 < cells);

    AccelDevice devs[16];
    const u32 nd = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < nd; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    struct Case {
        const char* what;
        const char* table;
        const char* blank;
        u32 lanes;     // 0: a CPU
        const char* device;
    };
    const Case cases[] = {
        {"a CPU (nova_v2)", "nova_v2", "INERT", 0, "cpu"},
        {"a CPU (nova_v1)", "nova_v1", "NOP0", 0, "cpu"},
        {"a CPU (nova_v1_plain)", "nova_v1_plain", "NOP0", 0, "cpu"},
        {"a CPU (nova_v1_inv2)", "nova_v1_inv2", "NOP0", 0, "cpu"},
        {"a 1-lane warp, the reference", "nova_v2", "INERT", 1, "cpu"},
        {"a 32-lane warp, the reference", "nova_v2", "INERT", 32, "cpu"},
        {"a 32-lane warp, the GPU (every round checked)", "nova_v2", "INERT", 32, "gpu"},
    };
    const u32 at = 100;
    for (const Case& c : cases) {
        if (strcmp(c.device, "gpu") == 0 && !gpu) {
            printf("  turing: no hardware GPU, the GPU's case skipped\n");
            continue;
        }
        const u32 stride = c.lanes ? c.lanes : 1;
        const TmProgram prog = tm_compile(bb4, stride, cells, start, c.blank);
        TextBuf cfg;
        cfg.append("seed = 3\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 4096\nworld.millicores = 1000\n"
                   "world.gpus = 64\ngpu.warps = true\nspark.initial = 0\nspark.rate = 0\nhazard.copy = 0\n"
                   "hazard.flaw = 0\nhazard.rot = 0\nheat.enabled = false\nmem.upkeep = 0\nsalvage.ratio = 0\n"
                   "warp.step_cost = 1\nwarp.lane_cost = 0\nwarp.capacity = 10000\nwarp.verify = 1\ncpu.instr_per_core = 10000\n");
        cfg.append("isa.table = ");
        cfg.append(c.table);
        cfg.append("\nwarp.lanes_min = ");
        cfg.append_u64(stride);
        cfg.append("\nwarp.lanes_max = ");
        cfg.append_u64(stride);
        cfg.append("\nwarp.device = ");
        cfg.append(c.device);
        cfg.append("\nseed.at = ");
        cfg.append_u64(at);
        cfg.append(c.lanes ? "\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 1\n"
                           : "\nseed.form = cpu\nseed.millicores = 1000\nseed.gpus = 0\n");
        cfg.append("seed.genome =");
        cfg.append(prog.names.view());
        cfg.append_char('\n');
        Run run;
        if (!make_run(run, cfg.c_str(), 4)) {
            CHECK(false);
            continue;
        }
        run.run_until(6); // (107 steps take some 4,000 instructions: a tick at these budgets)
        World& w = run.world();
        w.warp_sync(0);
        const Soup& s = w.node_soup(0);
        // Every lane's tape is the machine's, and the code stands at the halt.
        bool tape_ok = true;
        for (u32 i = 0; i < cells; ++i)
            for (u32 l = 0; l < stride; ++l)
                tape_ok = tape_ok && s.mem()[at + prog.tape + i * stride + l] == want[i];
        bool halted = s.body_count() == 1;
        if (halted) {
            const u32 b = s.body_at(0);
            if (c.lanes) {
                const u32 wp = s.warp_head(b);
                halted = wp != kNone;
                for (u32 l = 0; halted && l < stride; ++l)
                    halted = s.lane(wp, l).ip == at + prog.halt && !s.lane(wp, l).halted;
            } else {
                halted = s.cpu(s.body(b).cpus).ip == at + prog.halt;
            }
        }
        printf("  turing: the 4-state busy beaver (%u steps, %u marks) on %s: %s, %s\n", steps, marks, c.what,
               tape_ok ? "the same tape" : "a DIFFERENT tape", halted ? "halted" : "NOT halted");
        CHECK(tape_ok && halted);
        CHECK(w.warp_mismatches() == 0 && !w.warp_failed());
        if (strcmp(c.device, "gpu") == 0)
            CHECK(w.warp_gpu_rounds() > 0 && w.warp_verified() == w.warp_gpu_rounds());
    }
}

void suite_warp() {
    test_turing_machine();
    test_lockstep();
    test_divergence();
    test_offset_copy();
    test_read_then_write();
    test_own_body();
    test_host_instruction();
    test_block_ops_fail();
    test_launch();
    test_own_lanes();
    test_lanes_inherited();
    test_forms();
    test_gpu_sparks();
    test_death_rule();
    test_costs();
    test_engulf_takes_warps();
    test_gpu_replicator();
    test_view_warps();
    test_determinism();
    test_gpu_device();
    test_mixed_widths();
    test_gpu_div_loop();
    test_gpu_results_in_place();
    test_off_is_phase7();
}
