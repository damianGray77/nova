// The in-world NPU (Phase 2b, design.md 6.8): no NPU means everything NPU
// fails exactly as before; the unit ledger through ALLOC's selector 4; the
// four ops (dot, matvec, matmul, argmax) on signed fixed-point codons, all
// or nothing, with their MAC capacity, limits and costs; sensing; the
// shuffled maps; and the npu_user event, replayed exactly.
#include <cstdio>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// The NPU ops, by accelerator BX under the nearest-to-self numbering.
enum : u32 { kDot = 16, kMatvec, kMatmul, kArgmax };

// A quiet soup (1024 codons, 1000 millicores) with `npus` NPU units and one
// program planted at 100 holding 500 millicores and `held` NPU units.
struct Fix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;

    bool init(const char* prog, u32 npus, u32 held, const char* extra = "") {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.npus = ");
        cfg.append_u64(npus);
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
        s.claim_npus(b, held);
        s.prepare_gpu(); // sets NPU capacity too
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    i64 op(u32 bx, u32 cx, u32 rh, u32 wh, u32 ax = 0) {
        cpu().ip = 100;
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
        cpu().rh = rh;
        cpu().wh = wh;
        return s.run_cpu(c, 1);
    }
    void set(u32 at, const u8* v, u32 n) {
        for (u32 i = 0; i < n; ++i)
            s.mem_mut()[at + i] = v[i];
    }
};

void program(TextBuf& out, u32 n) {
    out.append("GPU");
    for (u32 i = 0; i < n; ++i)
        out.append(" INC");
}

// Without an NPU: the NPU ops, ALLOC/RELEASE selector 4 and SENSE 11-13 fail.
void no_npu() {
    Fix f;
    CHECK(f.init("GPU INC", 0, 0));
    CHECK(f.op(kDot, 1, 100, 101) == 1 && f.cpu().err == 1);
    CHECK(f.p.sel.res_n == 4 && f.p.sel.sense_n == 8 && !f.s.has_npu());
    Fix a;
    CHECK(a.init("ALLOC", 0, 0));
    a.op(4, 0, 0, 0);
    CHECK(a.cpu().err == 1);
    // A GPU but no NPU: SENSE has 11 readings, so 11-13 fail.
    Fix g;
    CHECK(g.init("SENSE", 0, 0, "world.gpus = 10\n"));
    g.op(11, 0, 0, 0);
    CHECK(g.cpu().err == 1 && g.p.sel.sense_n == 11);
}

// ALLOC and RELEASE with selector 4, the DIVIDE share and death.
void ledger() {
    Fix f;
    CHECK(f.init("ALLOC RELEASE", 100, 0));
    CHECK(f.p.sel.res_n == 5 && f.p.sel.sense_n == 14);
    f.op(4, 30, 0, 0);
    CHECK(f.cpu().err == 0 && f.s.npu_count(f.b) == 30 && f.s.free_npus() == 70);
    f.cpu().ax = 0;
    f.cpu().bx = 4;
    f.cpu().cx = 10;
    f.s.run_cpu(f.c, 1); // RELEASE NPU: 10
    CHECK(f.cpu().err == 0 && f.s.npu_count(f.b) == 20 && f.s.free_npus() == 80);
    CHECK(soup_ok(f.s));

    Fix d;
    CHECK(d.init("ALLOC DIVIDE", 100, 64));
    d.op(0, 8, 0, 0);       // ALLOC 8 codons (ip 100)
    d.cpu().bx = 0;
    d.cpu().cx = 32;
    d.s.run_cpu(d.c, 1);    // DIVIDE, share 32/64
    u32 daughter = d.s.body_at(d.s.body_count() - 1);
    CHECK(d.s.tally().births == 1 && d.s.npu_count(daughter) == 32 && d.s.npu_count(d.b) == 32);

    Fix k;
    CHECK(k.init("INC", 100, 0));
    u32 broke = plant(k.s, 300, "INC", 0);
    k.s.claim_npus(broke, 40);
    k.s.tick(0);
    CHECK(k.s.body_count() == 1 && k.s.free_npus() == 100 && soup_ok(k.s));
}

void ops() {
    TextBuf prog;
    program(prog, 15); // 100-115, own codons as data
    {
        Fix f; // dot: signed sum of products, both heads advance
        CHECK(f.init(prog.c_str(), 100, 1));
        const u8 a[3] = {33, 34, 29}, b[3] = {36, 37, 38}; // (1, 2, -3) . (4, 5, 6) = -4
        f.set(101, a, 3);
        f.set(108, b, 3);
        i64 cost = f.op(kDot, 3, 101, 108);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 0u - 4u && f.cpu().rh == 104 && f.cpu().wh == 111);
        CHECK(cost == 4 && f.s.tally().npu_macs == 3);
    }
    {
        Fix f; // argmax: the first of the largest; the read head left on it
        CHECK(f.init(prog.c_str(), 100, 1));
        const u8 v[4] = {5, 60, 60, 2};
        f.set(101, v, 4);
        f.op(kArgmax, 4, 101, 0);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 1 && f.cpu().rh == 102);
    }
    {
        Fix f; // matvec: v = (0.5, -0.5), M = [[31/32, 0], [-31/32, 31/32]]
        CHECK(f.init(prog.c_str(), 100, 1));
        const u8 v[2] = {48, 16}, m[4] = {63, 32, 1, 63};
        f.set(101, v, 2);
        f.set(104, m, 4);
        f.op(kMatvec, 2, 101, 110, 104);
        CHECK(f.cpu().err == 0 && f.s.mem()[110] == 47 && f.s.mem()[111] == 1);
        CHECK(f.cpu().rh == 103 && f.cpu().wh == 112 && f.s.tally().npu_macs == 4);
        // Saturation: (31 x 31 x 2) / 32 = 60 clamps to 31; (-32 x 31 x 2) / 32 to -32.
        const u8 v2[2] = {63, 63}, m2[4] = {63, 63, 0, 0};
        f.set(101, v2, 2);
        f.set(104, m2, 4);
        f.op(kMatvec, 2, 101, 110, 104);
        CHECK(f.s.mem()[110] == 63 && f.s.mem()[111] == 0);
    }
    {
        Fix f; // matmul: A ~ identity (31/32), B = [[16, -16], [8, -8]] / 32
        CHECK(f.init(prog.c_str(), 100, 1));
        const u8 a[4] = {63, 32, 32, 63}, b[4] = {48, 16, 40, 24};
        f.set(101, a, 4);
        f.set(105, b, 4);
        f.op(kMatmul, 2, 101, 110, 105);
        CHECK(f.cpu().err == 0 && f.s.mem()[110] == 47 && f.s.mem()[111] == 16 &&
              f.s.mem()[112] == 39 && f.s.mem()[113] == 24);
        CHECK(f.cpu().rh == 105 && f.cpu().wh == 114 && f.s.tally().npu_macs == 8);
    }
    {
        Fix f; // all or nothing: an output into waste writes nothing, uses nothing
        CHECK(f.init(prog.c_str(), 100, 1));
        const float cap = f.s.npu_capacity(f.b);
        const u8 before = f.s.mem()[300];
        f.op(kMatvec, 2, 101, 300, 104);
        CHECK(f.cpu().err == 1 && f.s.mem()[300] == before && f.s.npu_capacity(f.b) == cap);
        f.op(kMatvec, 2, 101, 110, 5000); // M outside the soup
        CHECK(f.cpu().err == 1);
    }
    {
        Fix f; // capacity: one unit = 256 MACs a tick; 7^3 = 343 is too many
        TextBuf big;
        program(big, 120);
        CHECK(f.init(big.c_str(), 100, 1));
        f.op(kMatmul, 7, 101, 101, 101);
        CHECK(f.cpu().err == 1);
        i64 cost = f.op(kMatmul, 6, 101, 101, 101); // 216 MACs: 4 + 216/64
        CHECK(f.cpu().err == 0 && cost == 4 + 3 && f.s.tally().npu_macs == 216);
        Fix g; // the size limits hold whatever the capacity
        CHECK(g.init(big.c_str(), 100, 100));
        g.op(kMatvec, 65, 101, 101, 101);
        CHECK(g.cpu().err == 1);
        g.op(kMatmul, 33, 101, 101, 101);
        CHECK(g.cpu().err == 1);
        g.op(kDot, 0, 101, 101);
        CHECK(g.cpu().err == 1);
    }
}

// SENSE 11-13: my NPU units, their mean heat, free NPU units on the node.
void sensing() {
    const u32 want[3] = {12, 40, 88};
    for (u32 r = 0; r < 3; ++r) {
        Fix f;
        CHECK(f.init("SENSE", 100, 12));
        for (u32 u = 0; u < 100; ++u)
            if (f.s.npu_owner(u) == f.b)
                f.s.set_npu_heat(u, 40.0f);
        f.op(11 + r, 0, 0, 0);
        CHECK(f.cpu().err == 0 && f.cpu().cx == want[r]);
    }
}

// Wear, through the same ledger code as the GPU's (whose heat tests cover the
// shared heat path).
void wear() {
    Fix f;
    CHECK(f.init("INC", 1000, 1000, "npu.wear = 0.01\n"));
    f.s.tick(1);
    const u32 worn = static_cast<u32>(f.s.tally().npu_worn);
    CHECK(worn > 3 && worn < 25 && f.s.npu_count(f.b) == 1000 - worn && soup_ok(f.s));
}

void shuffled_map() {
    SoupParams p;
    CHECK(make_params(&p, "world.npus = 10\nisa.selectors = shuffled\nseed = 3\n", 1024));
    u32 seen = 0, rseen = 0, sseen = 0;
    for (u32 i = 0; i < kNpuOps; ++i)
        seen |= 1u << p.sel.npu[i];
    for (u32 i = 0; i < 5; ++i)
        rseen |= 1u << p.sel.res[i];
    for (u32 i = 0; i < 14; ++i)
        sseen |= 1u << p.sel.sense[i];
    CHECK(seen == 15 && p.sel.npu[0] != 0 && rseen == 31 && p.sel.res[0] != 0);
    CHECK(sseen == (1u << 14) - 1 && p.sel.sense_n == 14 && p.sel.res_n == 5);
}

// (The NPU replicator is 21 codons and hands each daughter a third of its CPU,
// so under memory upkeep it needs a well-funded node to keep going.)
const char* const kNpuWorld = "seed = 6\nworld.model = soup\nworld.nodes = 1\nworld.soup_size = 8192\n"
                              "world.millicores = 4000\nworld.npus = 400\ncpu.instr_per_core = 5000\n"
                              "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                              "seed.at = 2000\nseed.millicores = 400\nseed.npus = 400\n";

// A replicator that uses the NPU: once its lineage meets the gate, its next
// NPU op is the npu_user event.
void npu_user() {
    TextBuf cfg;
    cfg.append(kNpuWorld);
    cfg.append("seed.genome = ");
    cfg.append(kNpuReplicator);
    cfg.append_char('\n');
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    u64 births = 0, macs = 0;
    for (u32 t = 0; t < 40; ++t) {
        run.step();
        births += run.world().last_soup_tick().births;
        macs += run.world().last_soup_tick().npu_macs;
    }
    CHECK(births > 10 && macs > 100);
    CHECK(run.detection().found && run.npu_detection().found);
    CHECK(run.npu_detection().tick >= run.detection().tick);
    CHECK(soup_ok(run.world().node_soup(0)));
    printf("  npu: seeded NPU-using replicator, 40 ticks: %llu births, %llu MACs, gate at tick %llu, "
           "npu_user at tick %llu\n",
           static_cast<unsigned long long>(births), static_cast<unsigned long long>(macs),
           static_cast<unsigned long long>(run.detection().tick),
           static_cast<unsigned long long>(run.npu_detection().tick));
}

// A world with both accelerators replays exactly (events included) and
// resumes from a snapshot.
void accel_replay() {
    const char* path = "nova_test_npu.replay";
    Run rec;
    CHECK(make_run(rec, kSoupWorldAccel, 20));
    TextBuf err;
    CHECK(rec.open_replay(path, 25, &err));
    rec.run_until(100);
    rec.finish();
    bool npu_event = false;
    for (usize i = 0; i < rec.event_count(); ++i)
        npu_event = npu_event || rec.event_text(i).starts_with("npu_user");
    CHECK(npu_event);
    ReplayLog log;
    CHECK(log.load_file(path, &err));
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r) && r.events_checked == rec.event_count());
    CHECK(replay_verify(log, 20, &r));
    remove(path);

    Run straight;
    CHECK(make_run(straight, kSoupWorldAccel, 1));
    straight.run_until(60);
    Run first;
    CHECK(make_run(first, kSoupWorldAccel, 20));
    first.run_until(30);
    ByteWriter bytes;
    first.snapshot(bytes);
    Run resumed;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
    resumed.run_until(60);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
}

void config_errors() {
    SoupParams p;
    CHECK(!make_params(&p, "npu.macs = 0\n", 1024));
    CHECK(!make_params(&p, "npu.wear = 2\n", 1024));
    CHECK(make_params(&p, "world.npus = 3\n", 1024) && p.npus == 3 && p.npu_macs == 256 &&
          p.npu_cpu_per_mac == 1.0 / 64.0 && p.npu_gain == 9.0f);
}

} // namespace

void suite_npu() {
    no_npu();
    ledger();
    ops();
    sensing();
    wear();
    shuffled_map();
    npu_user();
    accel_replay();
    config_errors();
}
