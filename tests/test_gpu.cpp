// The in-world GPU (Phase 2 and 2b, design.md 6.7-6.8): no GPU means
// everything GPU fails exactly as before; the unit ledger (claims, release,
// DIVIDE share, ENGULF, death, wear, heat release); the 16 ops -- reductions
// all-or-nothing, elementwise ops with partial work across ticks -- and their
// costs; sensing; the shuffled op map; and a seeded GPU replicator that copies
// itself with the GPU, is detected, and replays exactly.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// kQuiet with heat on.
const char* const kWarm = "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n";

// The GPU ops, by accelerator BX under the nearest-to-self numbering.
enum : u32 {
    kCompare = 0, kCount, kFind, kSum, kCopy, kFill, kNot, kRcopy,
    kAdd, kSub, kXor, kAnd, kOr, kMin, kMax, kMul
};

// A quiet soup (1024 codons, 1000 millicores) with `gpus` GPU units and one
// program planted at 100 holding 500 millicores and `held` GPU units.
struct Fix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;

    bool init(const char* prog, u32 gpus, u32 held, const char* extra = "", const char* base = kQuiet) {
        TextBuf cfg;
        cfg.append(base);
        cfg.append("world.millicores = 1000\nworld.gpus = ");
        cfg.append_u64(gpus);
        cfg.append_char('\n');
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 5, 0);
        b = plant(s, 100, prog, 500);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        s.claim_gpus(b, held);
        s.prepare_gpu();
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    i64 run(u32 n = 1) {
        i64 used = 0;
        for (u32 i = 0; i < n; ++i)
            used += s.run_cpu(c, 1);
        return used;
    }
    void regs(u32 ax, u32 bx, u32 cx) {
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
    }
    void heads(u32 rh, u32 wh) {
        cpu().rh = rh;
        cpu().wh = wh;
    }
    // One accelerator op from ip 100 with the given registers and heads.
    i64 op(u32 bx, u32 cx, u32 rh, u32 wh, u32 ax = 0) {
        cpu().ip = 100;
        regs(ax, bx, cx);
        heads(rh, wh);
        return run();
    }
    void set(u32 at, const u8* v, u32 n) {
        for (u32 i = 0; i < n; ++i)
            s.mem_mut()[at + i] = v[i];
    }
};

// A program of GPU then n INCs (its own codons 101.. serve as data).
void program(TextBuf& out, u32 n) {
    out.append("GPU");
    for (u32 i = 0; i < n; ++i)
        out.append(" INC");
}

// Without a GPU every GPU selector fails as in Phase 1, costing 1.
void no_gpu() {
    {
        Fix f;
        CHECK(f.init("GPU", 0, 0));
        f.regs(0, kCopy, 5);
        CHECK(f.run() == 1 && f.cpu().err == 1);
        CHECK(!f.s.has_gpu() && f.s.free_gpus() == 0 && f.p.sel.sense_n == 8 && f.p.sel.res_n == 4);
    }
    const char* fails[] = {"ALLOC", "RELEASE", "SENSE"};
    const u32 bx[] = {2, 2, 8};
    for (int i = 0; i < 3; ++i) {
        Fix f;
        CHECK(f.init(fails[i], 0, 0));
        f.regs(0, bx[i], 0); // even CX = 0, which succeeds for memory and CPU
        f.run();
        CHECK(f.cpu().err == 1);
    }
}

// Claims, release, the DIVIDE share, ENGULF and death move GPU units the way
// they move millicores, and every unit stays accounted for.
void ledger() {
    Fix f;
    CHECK(f.init("ALLOC RELEASE", 100, 0));
    f.regs(0, 2, 30);
    f.run(); // ALLOC GPU: 30 units from the pool
    CHECK(f.cpu().err == 0 && f.s.gpu_count(f.b) == 30 && f.s.free_gpus() == 70);
    f.regs(0, 2, 10);
    f.run(); // RELEASE GPU: 10, oldest first
    CHECK(f.cpu().err == 0 && f.s.gpu_count(f.b) == 20 && f.s.free_gpus() == 80);
    f.regs(0, 2, 500);
    f.cpu().ip = 100;
    f.run(); // asking for more than the pool takes what is there
    CHECK(f.s.gpu_count(f.b) == 100 && f.s.free_gpus() == 0);
    f.regs(0, 2, 1);
    f.cpu().ip = 100;
    f.run(); // an empty pool fails
    CHECK(f.cpu().err == 1);
    CHECK(soup_ok(f.s));

    Fix d; // DIVIDE hands over floor(units x CX / 64) of the GPU units too
    CHECK(d.init("ALLOC DIVIDE", 100, 64));
    d.regs(0, 0, 8);
    d.run(); // ALLOC 8 codons
    d.regs(0, 0, 16);
    d.run(); // DIVIDE, share 16/64
    u32 daughter = d.s.body_at(d.s.body_count() - 1);
    CHECK(d.s.tally().births == 1 && d.s.gpu_count(daughter) == 16 && d.s.gpu_count(d.b) == 48);
    CHECK(soup_ok(d.s));

    Fix e; // ENGULF takes every GPU unit, with its heat
    CHECK(e.init("ENGULF INC", 100, 10));
    u32 prey = plant(e.s, 200, "INC INC", 100);
    e.s.claim_gpus(prey, 20);
    for (u32 u = 0; u < 100; ++u)
        if (e.s.gpu_owner(u) == prey)
            e.s.set_gpu_heat(u, 33.0f);
    e.run();
    u32 hot = 0;
    for (u32 u = 0; u < 100; ++u)
        hot += e.s.gpu_owner(u) == e.b && e.s.gpu_heat(u) == 33.0f;
    CHECK(e.s.tally().engulfs == 1 && e.s.gpu_count(e.b) == 30 && hot == 20);
    e.s.death_sweep(); // the engulfed prey leaves the schedule here
    CHECK(soup_ok(e.s));

    Fix k; // death returns them to the pool; GPU units alone don't keep a body alive
    CHECK(k.init("INC", 100, 0));
    u32 broke = plant(k.s, 300, "INC", 0); // no millicore
    k.s.claim_gpus(broke, 40);
    k.s.tick(0);
    CHECK(k.s.body_count() == 1 && k.s.free_gpus() == 100 && soup_ok(k.s));
}

// The elementwise ops: copy, fill, not, rcopy and the dest-op-source family.
void elementwise() {
    const u8 src[6] = {3, 9, 17, 40, 41, 63};
    {
        Fix f; // copy: heads advance, the write head wraps at its block's end
        TextBuf prog;
        program(prog, 15);
        CHECK(f.init(prog.c_str(), 100, 4));
        f.set(101, src, 6);
        i64 cost = f.op(kCopy, 6, 101, 110);
        bool same = true;
        for (u32 i = 0; i < 6; ++i)
            same = same && f.s.mem()[110 + i] == src[i];
        CHECK(same && f.cpu().err == 0 && f.cpu().cx == 0);
        CHECK(f.cpu().rh == 107 && f.cpu().wh == 100); // 116 wraps to the block start
        CHECK(cost == 4 + 0 && f.s.tally().gpu_elements == 6);
    }
    {
        Fix f; // fill, not, rcopy
        TextBuf prog;
        program(prog, 23);
        CHECK(f.init(prog.c_str(), 100, 4));
        f.set(101, src, 6);
        f.op(kFill, 3, 0, 110, 70); // 70 mod 64 = 6, and the read head stays put
        CHECK(f.s.mem()[110] == 6 && f.s.mem()[112] == 6 && f.cpu().rh == 0 && f.cpu().cx == 0);
        f.op(kNot, 2, 101, 113);
        CHECK(f.s.mem()[113] == 63 - 3 && f.s.mem()[114] == 63 - 9);
        f.op(kRcopy, 6, 106, 115); // 106 down to 101: reversed
        bool reversed = true;
        for (u32 i = 0; i < 6; ++i)
            reversed = reversed && f.s.mem()[115 + i] == src[5 - i];
        CHECK(reversed && f.cpu().rh == 100);
        f.op(kRcopy, 2, 100, 121); // at the block's start the read head wraps to its end
        CHECK(f.cpu().rh == 122 && f.s.mem()[122] == f.s.mem()[123]);
    }
    {
        // dest <- dest op source, mod 64, against a source of {60, 5}.
        struct Case {
            u32 op;
            u8 d0, d1, w0, w1;
        };
        const Case cases[] = {
            {kAdd, 10, 5, 6, 10},  {kSub, 10, 5, 14, 0},  {kXor, 10, 5, 10 ^ 60, 0},
            {kAnd, 12, 7, 12 & 60, 5}, {kOr, 3, 2, 63, 7}, {kMin, 10, 9, 10, 5},
            {kMax, 10, 9, 60, 9},  {kMul, 3, 13, 180 % 64, 65 % 64},
        };
        bool all = true;
        for (const Case& k : cases) {
            Fix f;
            TextBuf prog;
            program(prog, 7);
            if (!f.init(prog.c_str(), 100, 4))
                return;
            const u8 s2[2] = {60, 5}, d2[2] = {k.d0, k.d1};
            f.set(101, s2, 2);
            f.set(104, d2, 2);
            f.op(k.op, 2, 101, 104);
            bool good = f.s.mem()[104] == k.w0 && f.s.mem()[105] == k.w1 && f.cpu().err == 0;
            if (!good)
                fprintf(stderr, "  gpu: op %u gave %u %u\n", k.op, f.s.mem()[104], f.s.mem()[105]);
            all = all && good;
        }
        CHECK(all);
    }
    {
        // Private code applies per element, and the op stops at the first one
        // it may not do: the read head runs out of waste into another body.
        Fix f;
        CHECK(f.init("GPU INC INC INC INC INC", 100, 4));
        CHECK(plant(f.s, 200, "INC INC INC", 100) != kNone);
        const u8 a = f.s.mem()[198], b = f.s.mem()[199];
        f.op(kCopy, 5, 198, 101);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 3 && f.s.mem()[101] == a && f.s.mem()[102] == b);
        f.op(kCopy, 3, 100, 300); // writing into waste: nothing done, fails
        CHECK(f.cpu().err == 1);
        f.op(kCopy, 3, 100, 200); // or into someone else
        CHECK(f.cpu().err == 1);
    }
    {
        // Capacity: one unit does 32 element-ops a tick; the rest waits in CX.
        Fix f;
        TextBuf prog;
        program(prog, 99);
        CHECK(f.init(prog.c_str(), 100, 1));
        i64 cost = f.op(kCopy, 50, 150, 120);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 18 && cost == 4 + 2); // 32 done, 32/16 = 2
        f.cpu().ip = 100;
        f.run(); // no capacity left this tick
        CHECK(f.cpu().err == 1 && f.cpu().cx == 18);
        f.s.prepare_gpu(); // next tick
        f.cpu().ip = 100;
        f.run();
        CHECK(f.cpu().err == 0 && f.cpu().cx == 0 && f.s.tally().gpu_elements == 50);
    }
}

// The reductions: compare, count, find, sum -- all-or-nothing.
void reductions() {
    Fix f;
    TextBuf prog;
    program(prog, 15);
    CHECK(f.init(prog.c_str(), 100, 1)); // 32 element-ops a tick
    const u8 v[6] = {7, 9, 7, 2, 7, 30};
    f.set(101, v, 6);
    f.set(108, v, 6);
    f.s.mem_mut()[110] = 1; // one difference
    f.op(kCompare, 6, 101, 108);
    CHECK(f.cpu().err == 0 && f.cpu().cx == 1 && f.cpu().rh == 107 && f.cpu().wh == 114);
    f.op(kCount, 6, 101, 0, 7);
    CHECK(f.cpu().cx == 3 && f.cpu().rh == 107);
    f.op(kSum, 6, 101, 0);
    CHECK(f.cpu().cx == 7 + 9 + 7 + 2 + 7 + 30);
    u64 before = f.s.tally().gpu_elements;
    f.op(kFind, 6, 101, 0, 2); // the 2 at offset 3: the read head stops on it
    CHECK(f.cpu().err == 0 && f.cpu().cx == 3 && f.cpu().rh == 104);
    CHECK(f.s.tally().gpu_elements == before + 4); // charged only up to the match
    f.op(kFind, 6, 101, 0, 50); // absent: fails, nothing changes
    CHECK(f.cpu().err == 1 && f.cpu().rh == 101 && f.cpu().cx == 6);
    // All-or-nothing: more elements than the capacity left, or one unreadable.
    f.op(kSum, 20, 101, 0); // 32 - 6 - 6 - 6 - 4 = 10 left
    CHECK(f.cpu().err == 1 && f.cpu().cx == 20 && f.cpu().rh == 101);
    CHECK(plant(f.s, 200, "INC INC", 100) != kNone);
    f.s.prepare_gpu();
    f.op(kCount, 4, 198, 0, 7); // 198, 199 waste, then someone's code
    CHECK(f.cpu().err == 1 && f.cpu().rh == 198);
}

void failures() {
    // No units, CX <= 0, or a BX past the ops (16-19 is the NPU, absent here).
    Fix f;
    CHECK(f.init("GPU INC", 100, 0));
    CHECK(f.op(kCopy, 3, 100, 101) == 1 && f.cpu().err == 1);
    Fix g;
    CHECK(g.init("GPU INC", 100, 2));
    CHECK(g.op(kCopy, 0, 100, 101) == 1 && g.cpu().err == 1);
    CHECK(g.op(16, 3, 100, 101) == 1 && g.cpu().err == 1);
    CHECK(g.op(20, 3, 100, 101) == 1 && g.cpu().err == 1);
}

// SENSE 8-10: my GPU units, their mean heat, free GPU units on the node.
void sensing() {
    const u32 want[3] = {12, 50, 88};
    for (u32 r = 0; r < 3; ++r) {
        Fix f;
        CHECK(f.init("SENSE", 100, 12));
        for (u32 u = 0; u < 100; ++u)
            if (f.s.gpu_owner(u) == f.b)
                f.s.set_gpu_heat(u, 50.0f);
        f.regs(0, 8 + r, 0);
        f.run();
        CHECK(f.cpu().err == 0 && f.cpu().cx == want[r]);
    }
}

// Heat: a flat-out GPU heats by heat.gpu_gain a tick and throttles; past
// max its units are freed, hot. Wear returns held units too.
void heat_and_wear() {
    {
        Fix f;
        CHECK(f.init("INC", 100, 10, "", kWarm));
        for (u32 u = 0; u < 100; ++u)
            if (f.s.gpu_owner(u) == f.b)
                f.s.set_gpu_heat(u, 150.0f);
        f.s.tick(1);
        CHECK(f.s.tally().gpu_released == 10 && f.s.gpu_count(f.b) == 0 && f.s.free_gpus() == 100);
        bool kept = true;
        for (u32 u = 0; u < 100; ++u)
            kept = kept && (f.s.gpu_heat(u) == 0.0f || f.s.gpu_heat(u) > 100.0f);
        CHECK(kept); // freed units keep their heat
    }
    {
        // A body that uses its whole GPU capacity every tick (copying its own
        // codons onto themselves) heats up, throttles, and settles in the
        // throttle band: h = 90 x speed(h), about 71.
        Fix f;
        TextBuf prog;
        prog.append("GPU");
        for (int i = 0; i < 63; ++i)
            prog.append(" NOP0");
        CHECK(f.init(prog.c_str(), 100, 1, "", kWarm));
        u32 unit = kNone;
        for (u32 u = 0; u < 100; ++u)
            if (f.s.gpu_owner(u) == f.b)
                unit = u;
        f.regs(0, kCopy, 1000000);
        f.heads(132, 132);
        for (u64 t = 1; t < 60; ++t)
            f.s.tick(t);
        float h = f.s.gpu_heat(unit);
        CHECK(f.s.gpu_count(f.b) == 1 && h > 65.0f && h < 78.0f); // hot, throttled, held
        f.s.prepare_gpu();
        CHECK(f.s.gpu_capacity(f.b) > 20.0f && f.s.gpu_capacity(f.b) < 32.0f);
    }
    {
        Fix f;
        CHECK(f.init("INC", 1000, 1000, "gpu.wear = 0.01\n"));
        f.s.tick(1);
        u32 worn = static_cast<u32>(f.s.tally().gpu_worn);
        CHECK(worn > 3 && worn < 25 && f.s.gpu_count(f.b) == 1000 - worn && soup_ok(f.s));
    }
}

// The shuffled control maps the GPU ops too, only when there is a GPU.
void shuffled_map() {
    SoupParams with, without;
    CHECK(make_params(&with, "world.gpus = 10\nisa.selectors = shuffled\nseed = 3\n", 1024));
    CHECK(make_params(&without, "isa.selectors = shuffled\nseed = 3\n", 1024));
    u32 seen = 0;
    for (u32 i = 0; i < kGpuOps; ++i)
        seen |= 1u << with.sel.gpu[i];
    CHECK(seen == 0xFFFF && with.sel.gpu[0] != 0 && with.sel.sense_n == 11);
    u32 sseen = 0;
    for (u32 i = 0; i < 11; ++i)
        sseen |= 1u << with.sel.sense[i];
    CHECK(sseen == (1u << 11) - 1 && with.sel.sense[0] != 0);
    // Without a GPU: SENSE over 8 readings and the op map left alone.
    CHECK(without.sel.sense_n == 8 && without.sel.gpu[0] == 0 && without.sel.gpu[15] == 15);
    bool eight = true;
    for (u32 i = 0; i < 8; ++i)
        eight = eight && without.sel.sense[i] < 8;
    CHECK(eight);
}

const char* const kGpuWorld = "seed = 6\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 8192\n"
                              "world.millicores = 4000\nworld.gpus = 400\ncpu.instr_per_core = 1000\n"
                              "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                              "seed.at = 2000\nseed.millicores = 400\nseed.gpus = 400\n";

// The faster polymerase: the seeded GPU replicator copies itself exactly,
// its lineage meets the gate, and the first GPU link is the gpu_replicator event.
void gpu_replicator() {
    TextBuf cfg;
    cfg.append(kGpuWorld);
    cfg.append("seed.genome = ");
    cfg.append(kGpuReplicator);
    cfg.append_char('\n');
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    u64 births = 0;
    for (u32 t = 0; t < 30; ++t) {
        run.step();
        births += run.world().last_soup_tick().births;
    }
    const Soup& s = run.world().node_soup(0);
    Array<u8> code;
    TextBuf err;
    CHECK(genome_from_names(s.params().table, kGpuReplicator, &code, &err));
    const u32 len = static_cast<u32>(code.size());
    u32 exact = 0;
    for (usize k = 0; k < s.body_count(); ++k) {
        const Block& bl = s.block(s.body(s.body_at(k)).blocks);
        bool same = bl.len == len;
        for (u32 i = 0; i < len && same; ++i)
            same = s.mem()[bl.start + i] == code[i];
        exact += same;
    }
    const Lineages* L = run.world().node_lineages(0);
    CHECK(births > 20 && exact > 10);
    CHECK(L && L->gpu_links() > 10 && run.detection().found && run.gpu_detection().found);
    CHECK(soup_ok(s));
    printf("  gpu: seeded GPU replicator, 30 ticks: %llu births, %u exact copies alive, gate at "
           "tick %llu, gpu_replicator at tick %llu\n",
           static_cast<unsigned long long>(births), exact,
           static_cast<unsigned long long>(run.detection().tick),
           static_cast<unsigned long long>(run.gpu_detection().tick));
}

// GPU worlds replay exactly at 1 and 20 threads (events included) and resume
// from snapshots at any thread count.
void replay_and_snapshot() {
    const char* path = "nova_test_gpu.replay";
    Run rec;
    CHECK(make_run(rec, kSoupWorldGpu, 20));
    TextBuf err;
    CHECK(rec.open_replay(path, 25, &err));
    rec.run_until(100);
    rec.finish();
    bool gpu_event = false;
    for (usize i = 0; i < rec.event_count(); ++i)
        gpu_event = gpu_event || rec.event_text(i).starts_with("gpu_replicator");
    CHECK(gpu_event);
    ReplayLog log;
    CHECK(log.load_file(path, &err));
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r) && r.events_checked == rec.event_count());
    CHECK(replay_verify(log, 20, &r));
    remove(path);

    Run straight;
    CHECK(make_run(straight, kSoupWorldGpu, 1));
    straight.run_until(80);
    Run first;
    CHECK(make_run(first, kSoupWorldGpu, 20));
    first.run_until(40);
    ByteWriter bytes;
    first.snapshot(bytes);
    Run resumed;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
    resumed.run_until(80);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    bool ok = true;
    for (u32 n = 0; n < resumed.world().node_count() && ok; ++n)
        ok = soup_ok(resumed.world().node_soup(n));
    CHECK(ok);
}

void config_errors() {
    SoupParams p;
    CHECK(!make_params(&p, "gpu.elements = 0\n", 1024));
    CHECK(!make_params(&p, "gpu.wear = 2\n", 1024));
    CHECK(!make_params(&p, "heat.gpu.warm = 80\nheat.gpu.hot = 60\n", 1024));
    CHECK(make_params(&p, "world.gpus = 7\n", 1024) && p.gpus == 7 && p.gpu_elements == 32 &&
          p.gpu_setup == 4 && p.gpu_cpu_per_element == 0.0625 && p.gpu_gain == 9.0f);
}

} // namespace

void suite_gpu() {
    no_gpu();
    ledger();
    elementwise();
    reductions();
    failures();
    sensing();
    heat_and_wear();
    shuffled_map();
    gpu_replicator();
    replay_and_snapshot();
    config_errors();
}
