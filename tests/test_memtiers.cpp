// Memory tiers (docs/world-plan.md W3): the first mem.fast_share of each
// node's soup is fast memory, the rest slow. With a timed cost table every
// load, store, template scan and bulk move costs its codon's tier's price --
// on a CPU its nearest cache's for fast memory and 1 MB's for slow, on a GPU
// lane its nearest cache's and the L2's -- each tier's divided by its
// substrate speed. ALLOC's resource 7 claims fast memory only, and fast pages
// are wiped mem.fast_volatility times as often. Every other suite runs with
// mem.fast_share = 0 (the test runner's legacy defaults), as every world did
// before.
#include <cmath>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/hash.h"
#include "nova/io.h"
#include "nova/replay.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

#include "../core/src/warp_layout.inc" // the record's tier words (WB_FAST ...)

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

// A quiet soup on home_v1 with memory tiers: 1,024 codons, the first 128 fast.
struct MFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra = "", u32 size = 1024) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\nspark.initial = 0\n");
        if (!strstr(extra, "cost.table"))
            cfg.append("cost.table = home_v1\n");
        if (!strstr(extra, "mem.fast_share"))
            cfg.append("mem.fast_share = 0.125\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
    Cpu& cpu(u32 b) { return s.cpu_mut(s.body(b).cpus); }
    u32 cpu_of(u32 b) const { return s.body(b).cpus; }
};

// n copies of an Op name, space separated.
void repeat(TextBuf* out, const char* op, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        if (out->size())
            out->append_char(' ');
        out->append(op);
    }
}

// A table price at a tier, as Exec prices it (the slow tier x load_far / load).
double tier_ps(const CostTable& t, double ps, bool fast, double fspeed = 1.0, double sspeed = 1.0) {
    return fast ? ps / fspeed : ps * t.load_far / t.load / sspeed;
}
u64 round_ps(double v) { return static_cast<u64>(v + 0.5); }

// --- the keys and the boundary -------------------------------------------------------

void test_keys() {
    // The boundary: whole pages, at least one, the whole soup at share 1.
    CHECK(mem_fast_end(32768, 0.125) == 4096 && mem_fast_end(1024, 0.125) == 128);
    CHECK(mem_fast_end(1000, 0.125) == 64 && mem_fast_end(100, 0.1) == 64); // 125 -> a page; 10 -> one page
    CHECK(mem_fast_end(32, 0.5) == 0 && mem_fast_end(4096, 0) == 0 && mem_fast_end(1000, 1) == 1000);
    // The engine's defaults (no test fallback): an eighth, wiped 4x as often, ALLOC's resource 7.
    CHECK(Config::set_fallback(nullptr, nullptr));
    SoupParams d;
    const bool made = make_params(&d, "", 32768);
    CHECK(made);
    if (made)
        CHECK(d.mem_fast_share == 0.125 && d.fast_volatility == 4.0 && d.fast_end == 4096 && d.sel.res_n == 8 &&
              d.mem_fast_speed == 1.0 && d.mem_slow_speed == 1.0);
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    // The test runner's legacy defaults: one tier, ALLOC as before.
    SoupParams l;
    CHECK(make_params(&l, "", 1024) && l.fast_end == 0 && l.sel.res_n == 4);
    // The shuffled control: without tiers exactly the old maps; with them a
    // permutation of all eight resources.
    SoupParams a, b, c;
    CHECK(make_params(&a, "isa.selectors = shuffled\n", 1024));
    CHECK(make_params(&b, "isa.selectors = shuffled\nmem.fast_share = 0\n", 1024));
    CHECK(make_params(&c, "isa.selectors = shuffled\nmem.fast_share = 0.25\n", 1024));
    CHECK(memcmp(&a.sel, &b.sel, sizeof(SelectorMap)) == 0 && a.sel.res_n == 4);
    u32 seen = 0;
    for (u32 i = 0; i < c.sel.res_n; ++i)
        seen |= 1u << c.sel.res[i];
    CHECK(c.sel.res_n == 8 && seen == 255 && c.sel.res[0] != 0 && c.fast_end == 256);
    // The table's new column: a load from 1 MB, the P-core's measured, the E-core's scaled.
    CostTable h = {}, e = {}, k = {};
    CHECK(builtin_costs("home_v1", &h) && builtin_costs_e("home_v1", &e) && builtin_costs("classic", &k));
    CHECK(h.load_far == 3600 && e.load_far == 4158 && k.load_far == 0);
    CHECK(h.op[static_cast<u32>(Op::copy)] == h.load + h.store); // (the recipes don't use it)
    // A bad value is refused.
    SoupParams bad;
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse("mem.fast_share = 1.5\n", &pe));
    CHECK(!soup_params_from_config(cfg, 1024, &bad));
}

// --- the CPU's prices ---------------------------------------------------------------

// Runs body b's CPU one instruction at a time, n times; each charge into out.
void charges(MFix& f, u32 b, u32 n, u64* out, u8 kind = kKindP) {
    for (u32 i = 0; i < n; ++i)
        out[i] = static_cast<u64>(f.s.run_cpu(f.cpu_of(b), 1, kind));
}

// A CPU copy costs each tier's price: a load at its source codon's tier and a
// store at its destination's. 32 codons cost 27,200 ps (143 adds) in fast
// memory and 129,696 ps (683 adds) in slow.
void test_cpu_copy() {
    MFix f;
    CHECK(f.init());
    const CostTable& t = f.p.cost;
    CHECK(f.p.fast_end == 128 && t.load == 755 && t.store == 95 && t.load_far == 3600);
    TextBuf prog;
    repeat(&prog, "COPY", 32);
    repeat(&prog, "NOP0", 32);
    const u32 fast = plant(f.s, 0, prog.c_str(), 1000);   // [0, 64): fast
    const u32 slow = plant(f.s, 600, prog.c_str(), 1000); // [600, 664): slow
    CHECK(fast != kNone && slow != kNone);
    f.cpu(fast).rh = 32; // copying its NOPs onto themselves
    f.cpu(fast).wh = 32;
    f.cpu(slow).rh = 632;
    f.cpu(slow).wh = 632;
    u64 cf[32], cs[32];
    charges(f, fast, 32, cf);
    charges(f, slow, 32, cs);
    u64 sf = 0, ss = 0;
    bool each = true;
    for (u32 i = 0; i < 32; ++i) {
        each = each && cf[i] == 850 && cs[i] == 4053;
        sf += cf[i];
        ss += cs[i];
    }
    CHECK(each);
    CHECK(cf[0] == round_ps(tier_ps(t, t.load, true) + tier_ps(t, t.store, true)));
    CHECK(cs[0] == round_ps(tier_ps(t, t.load, false) + tier_ps(t, t.store, false)));
    CHECK(sf == 27200 && ss == 129696);
    printf("  a CPU copy of 32 codons: %llu ps (%.0f adds) in fast memory, %llu ps (%.0f adds) in slow\n",
           static_cast<unsigned long long>(sf), static_cast<double>(sf) / t.alu, static_cast<unsigned long long>(ss),
           static_cast<double>(ss) / t.alu);

    // Across the boundary: a body on [112, 176) reads fast and writes slow
    // (755 + 453 = 1,208), and the other way (3,600 + 95 = 3,695).
    MFix g;
    CHECK(g.init());
    const u32 x = plant(g.s, 112, prog.c_str(), 1000);
    CHECK(x != kNone);
    g.cpu(x).rh = 112;
    g.cpu(x).wh = 144;
    u64 c1[2];
    charges(g, x, 2, c1);
    CHECK(c1[0] == 1208 && c1[1] == 1208);
    g.cpu(x).rh = 150;
    g.cpu(x).wh = 120;
    charges(g, x, 2, c1);
    CHECK(c1[0] == 3695 && c1[1] == 3695);

    // READ and WRITE: a load, a store, at their tiers.
    MFix r;
    CHECK(r.init());
    const u32 rf = plant(r.s, 16, "READ WRITE NOP0 NOP0", 1000), rs = plant(r.s, 700, "READ WRITE NOP0 NOP0", 1000);
    r.cpu(rf).rh = r.cpu(rf).wh = 18;
    r.cpu(rs).rh = r.cpu(rs).wh = 702;
    u64 c2[2], c3[2];
    charges(r, rf, 2, c2);
    charges(r, rs, 2, c3);
    CHECK(c2[0] == 755 && c2[1] == 95 && c3[0] == 3600 && c3[1] == 453);

    // A template search: JMP NOP0 past two INCs reads 1 template codon and
    // looks at 3 places, at their tiers; its own add and the jump are no
    // memory's (190 + 755 + 3 x 275 + 4,560 fast; 190 + 7,534 + 4,560 slow).
    MFix j;
    CHECK(j.init());
    const u32 jf = plant(j.s, 40, "JMP NOP0 INC INC NOP1", 1000), js = plant(j.s, 800, "JMP NOP0 INC INC NOP1", 1000);
    u64 c4[1], c5[1];
    charges(j, jf, 1, c4);
    charges(j, js, 1, c5);
    CHECK(c4[0] == 6330 && c5[0] == 190 + round_ps(tier_ps(t, t.load + 3.0 * t.scan, false)) + 4560 &&
          c5[0] == 12284);

    // ALLOC's codons claimed: a move each at their tier (16 x 16 = 256 fast,
    // 1,221 slow) beside its kernel call.
    MFix a;
    CHECK(a.init());
    const u32 al = plant(a.s, 600, "ALLOC ALLOC NOP0", 1000);
    a.cpu(al).bx = 7; // fast memory
    a.cpu(al).cx = 16;
    u64 c6[1];
    charges(a, al, 1, c6);
    CHECK(c6[0] == t.alu + t.call + 256);
    a.cpu(al).bx = 0; // anywhere: here, beside it, slow
    charges(a, al, 1, c6);
    CHECK(c6[0] == t.alu + t.call + round_ps(tier_ps(t, 16.0 * t.move, false)) && c6[0] == t.alu + t.call + 1221);

    // An E-core at its own prices: (872 + 109) x 4,158 / 872 = 4,678 slow.
    MFix e;
    CHECK(e.init());
    const u32 es = plant(e.s, 600, prog.c_str(), 1000);
    e.cpu(es).rh = e.cpu(es).wh = 632;
    u64 c7[1];
    charges(e, es, 1, c7, kKindE);
    CHECK(c7[0] == 4678 && e.p.ecost.load == 872 && e.p.ecost.store == 109);

    // Without tiers the same slow copy is the table's 850, exactly as before.
    MFix o;
    CHECK(o.init("mem.fast_share = 0\n"));
    const u32 os = plant(o.s, 600, prog.c_str(), 1000);
    o.cpu(os).rh = o.cpu(os).wh = 632;
    u64 c8[1];
    charges(o, os, 1, c8);
    CHECK(o.p.fast_end == 0 && c8[0] == 850);
    // Classic costs charge nothing for tiers: an instruction is 1.
    MFix k;
    CHECK(k.init("cost.table = classic\n"));
    const u32 ks = plant(k.s, 600, prog.c_str(), 1000);
    u64 c9[1];
    charges(k, ks, 1, c9);
    CHECK(c9[0] == 1);
    CHECK(soup_ok(f.s) && soup_ok(a.s) && soup_ok(j.s));
}

// A 2x fast-memory substrate halves that tier's cost and the floor (0.1)
// makes it ten times; the slow tier the same with its own speed.
void test_speeds() {
    const double speeds[3] = {1.0, 2.0, 0.1};
    u64 fast[3] = {}, slow[3] = {};
    for (u32 i = 0; i < 3; ++i) {
        MFix f;
        CHECK(f.init());
        f.p.mem_fast_speed = speeds[i];
        f.p.mem_slow_speed = speeds[i];
        TextBuf prog;
        repeat(&prog, "COPY", 4);
        const u32 bf = plant(f.s, 0, prog.c_str(), 1000), bs = plant(f.s, 600, prog.c_str(), 1000);
        u64 c[1];
        charges(f, bf, 1, c);
        fast[i] = c[0];
        charges(f, bs, 1, c);
        slow[i] = c[0];
    }
    CHECK(fast[0] == 850 && fast[1] == 425 && fast[2] == 8500);
    CHECK(slow[0] == 4053 && (slow[1] == 2026 || slow[1] == 2027) && slow[2] == 40530); // (4,052.98: rounding)
    // Only its own tier: a slow-memory speed leaves fast memory alone.
    MFix g;
    CHECK(g.init());
    g.p.mem_slow_speed = 2.0;
    const u32 b = plant(g.s, 0, "COPY COPY", 1000);
    u64 c[1];
    charges(g, b, 1, c);
    CHECK(c[0] == 850);
    printf("  a copy at fast-memory speeds 1, 2, 0.1: %llu, %llu, %llu ps; slow: %llu, %llu, %llu ps\n",
           static_cast<unsigned long long>(fast[0]), static_cast<unsigned long long>(fast[1]),
           static_cast<unsigned long long>(fast[2]), static_cast<unsigned long long>(slow[0]),
           static_cast<unsigned long long>(slow[1]), static_cast<unsigned long long>(slow[2]));
}

// --- warps ---------------------------------------------------------------------------

// A timed soup with warps and memory tiers: 1,024 codons, the first 128 fast;
// a tick of 4.75 us.
struct WFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra = "") {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 2000\nworld.gpus = 1000\ngpu.warps = true\ncost.table = home_v1\n"
                   "cpu.instr_per_core = 25000\nspark.initial = 0\n");
        if (!strstr(extra, "mem.fast_share"))
            cfg.append("mem.fast_share = 0.125\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
    u32 plant_gpu(u32 at, const char* prog) {
        Array<u8> code;
        TextBuf err;
        if (!genome_from_names(p.table, prog, &code, &err))
            return kNone;
        for (usize i = 0; i < code.size(); ++i)
            s.mem_mut()[at + i] = code[i];
        return s.create_body_form(at, static_cast<u32>(code.size()), 0, 64, kFormGpu);
    }
};

// The bodies the warp tests run, each a warp of 32 lanes: COPY steps in fast,
// mixed and slow memory, READ and WRITE in slow, a search in each tier.
struct WarpSet {
    u32 copy_fast, copy_mixed, copy_slow, read_slow, write_slow, jmp_fast, jmp_slow;
};
bool plant_set(WFix& f, WarpSet* w) {
    TextBuf copy, read, write;
    repeat(&copy, "COPY", 64);
    repeat(&read, "READ", 64);
    repeat(&write, "WRITE", 64);
    const char* jmp = "JMP NOP0 INC INC INC INC INC INC INC INC INC INC NOP1";
    w->copy_fast = f.plant_gpu(0, copy.c_str());     // [0, 64): every lane's codons fast
    w->jmp_fast = f.plant_gpu(64, jmp);              // [64, 77)
    w->copy_mixed = f.plant_gpu(96, copy.c_str());   // [96, 160): its heads cross the boundary each step
    w->read_slow = f.plant_gpu(200, read.c_str());   // [200, 264)
    w->write_slow = f.plant_gpu(300, write.c_str()); // [300, 364)
    w->copy_slow = f.plant_gpu(512, copy.c_str());   // [512, 576)
    w->jmp_slow = f.plant_gpu(700, jmp);             // [700, 713)
    const u32* all = &w->copy_fast;
    for (u32 i = 0; i < 7; ++i)
        if (all[i] == kNone)
            return false;
    return true;
}

// Steps a warp takes in a tick of `tick` ps when its steps cost `t[i % n]`,
// each checked against the clock before it runs.
u64 steps_in(double tick, const u64* t, u32 n) {
    u64 used = 0, k = 0;
    while (used + t[k % n] <= tick)
        used += t[k++ % n];
    return k;
}

// A warp's step costs its slowest lane's memory traffic at each codon's tier:
// on a GPU lane a load is 18,800 ps fast and 145,700 slow (the L2: today's
// price), a store 180 and 1,393, a place searched 4,219 and 32,700. In slow
// memory at speed 1 every step costs what it did before tiers.
void test_warp_copy() {
    WFix f;
    CHECK(f.init());
    WarpSet w;
    CHECK(plant_set(f, &w));
    f.s.tick(0);
    const CostTable& t = f.p.cost;
    const double tick = f.p.tick_units;
    CHECK(tick == 4750000.0 && t.gpu_l1 == 18800 && t.gpu_mem == 145700 && t.gpu_alu == 1393);
    const u64 copy_fast = 18800 + 180, copy_slow = 145700 + 1393;
    CHECK(copy_slow == t.gpu_op[static_cast<u32>(Op::copy)]); // today's COPY
    auto steps = [&](u32 b) { return f.s.warp(f.s.warp_head(b)).steps; };
    CHECK(steps(w.copy_fast) == steps_in(tick, &copy_fast, 1) && steps(w.copy_fast) == 250);
    CHECK(steps(w.copy_slow) == steps_in(tick, &copy_slow, 1) && steps(w.copy_slow) == 32);
    const u64 mixed[2] = {copy_fast, copy_slow}; // its heads alternate between the tiers
    CHECK(steps(w.copy_mixed) == steps_in(tick, mixed, 2) && steps(w.copy_mixed) == 57);
    const u64 read = 145700, write = 1393;
    CHECK(steps(w.read_slow) == steps_in(tick, &read, 1) && steps(w.write_slow) == steps_in(tick, &write, 1));
    // A search's places and template codon after its step: 1 + 11 at 4,219
    // fast, at 32,700 slow (the step that runs past the clock is carried over).
    const u64 jf = static_cast<u64>((tick - 1393) / (1393 + 12 * 4219)) + 1;
    const u64 js = static_cast<u64>((tick - 1393) / (1393 + 12 * 32700)) + 1;
    CHECK(steps(w.jmp_fast) == jf && steps(w.jmp_slow) == js);
    printf("  warp steps a tick: COPY %llu fast, %llu across, %llu slow; JMP %llu fast, %llu slow\n",
           static_cast<unsigned long long>(steps(w.copy_fast)), static_cast<unsigned long long>(steps(w.copy_mixed)),
           static_cast<unsigned long long>(steps(w.copy_slow)), static_cast<unsigned long long>(steps(w.jmp_fast)),
           static_cast<unsigned long long>(steps(w.jmp_slow)));
    CHECK(soup_ok(f.s));

    // Without tiers every COPY step costs today's 147,093 wherever it is.
    WFix o;
    CHECK(o.init("mem.fast_share = 0\n"));
    WarpSet v;
    CHECK(plant_set(o, &v));
    o.s.tick(0);
    CHECK(o.s.warp(o.s.warp_head(v.copy_fast)).steps == 32 && o.s.warp(o.s.warp_head(v.copy_mixed)).steps == 32);
    CHECK(o.s.warp(o.s.warp_head(v.jmp_fast)).steps == js);

    // The record carries the node's prices, divided by its speeds: fast
    // memory at 2x halves the fast tier's (a load 9,400, a copy step 9,490).
    WFix g;
    CHECK(g.init());
    g.p.mem_fast_speed = 2.0;
    WarpSet u;
    CHECK(plant_set(g, &u));
    g.s.tick_first(0);
    CHECK(g.s.warp_pack(0));
    const u32* I = g.s.warp_image_view().data();
    const u32 rec = g.s.warp_records()[0];
    CHECK(I[rec + WB_FAST] == 128 && I[rec + WB_TLOAD] == 9400 && I[rec + WB_TLOAD + 1] == 145700 &&
          I[rec + WB_TSTORE] == 90 && I[rec + WB_TSTORE + 1] == 1393 && I[rec + WB_TSCAN] == 2110 &&
          I[rec + WB_TSCAN + 1] == 32700);
    g.s.warp_run_ref();
    g.s.warp_unpack();
    g.s.warp_host(0);
    CHECK(g.s.warp(g.s.warp_head(u.copy_fast)).steps == static_cast<u64>(tick / 9490));
}

// The real GPU runs a tiered round word for word as the reference does:
// DXC's kernel (where the build has it) and FXC's.
void test_warp_device() {
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 dev = kNone;
    for (u32 i = 0; i < n && dev == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            dev = i;
    if (dev == kNone) {
        printf("  memtiers: no hardware GPU, the device check skipped\n");
        return;
    }
    for (u32 kind = 0; kind < 2; ++kind) {
        WarpGpu gpu;
        TextBuf err;
        WarpKernelOptions kopt;
        kopt.fxc = kind == 1;
        CHECK(gpu.open(dev, 32, &err, nullptr, &kopt));
        if (!gpu.is_open()) {
            printf("  memtiers: GPU open failed: %s\n", err.c_str());
            return;
        }
        for (u32 variant = 0; variant < 2; ++variant) { // speed 1, then a slow substrate on both tiers
            WFix r, g;
            CHECK(r.init() && g.init());
            if (variant == 1) {
                r.p.mem_fast_speed = g.p.mem_fast_speed = 0.5;
                r.p.mem_slow_speed = g.p.mem_slow_speed = 1.5;
            }
            WarpSet a, b;
            CHECK(plant_set(r, &a) && plant_set(g, &b));
            r.s.tick_first(0);
            g.s.tick_first(0);
            CHECK(r.s.warp_pack(0) && g.s.warp_pack(0));
            r.s.warp_run_ref();
            Array<u32> send;
            for (u32 x = 0; x < g.s.warp_slots(); ++x)
                send.push(x);
            WarpJob j = {};
            j.node = 0;
            j.img = g.s.warp_image().data();
            j.img_words = static_cast<u32>(g.s.warp_image().size());
            j.recs = g.s.warp_records().data();
            j.nrec = static_cast<u32>(g.s.warp_records().size());
            j.lanes = g.s.lanes_mut().data();
            j.nlanes = static_cast<u32>(g.s.lanes_mut().size());
            j.send = send.data();
            j.nsend = static_cast<u32>(send.size());
            CHECK(gpu.run(g.s.warp_consts().data(), static_cast<u32>(g.s.warp_consts().size()), &j, 1, &err));
            Array<WarpFetch> fl;
            for (u32 x = 0; x < g.s.warp_slots(); ++x)
                fl.push(WarpFetch{0, x, g.s.lanes_mut().data() + static_cast<usize>(x) * 32});
            CHECK(gpu.fetch(fl.data(), static_cast<u32>(fl.size()), &err));
            const Array<u32>& ri = r.s.warp_image_view();
            const Array<u32>& gi = g.s.warp_image_view();
            const bool same_img = ri.size() == gi.size() && memcmp(ri.data(), gi.data(), ri.size() * 4) == 0;
            const bool same_lanes = r.s.lanes_mut().size() == g.s.lanes_mut().size() &&
                                    memcmp(r.s.lanes_mut().data(), g.s.lanes_mut().data(),
                                           r.s.lanes_mut().size() * sizeof(Lane)) == 0;
            CHECK(same_img && same_lanes);
            // ... and the steps it ran are the reference's (the tiers' times, not 0)
            u64 tused = 0;
            const u32* I = gi.data();
            for (usize k = 0; k < g.s.warp_records().size(); ++k)
                tused += I[g.s.warp_records()[k] + WB_WARPS + WW_TUSED];
            CHECK(tused > 0);
            if (kind == 1 || variant == 1)
                continue;
            printf("  memtiers on the GPU: a tiered round of 7 bodies, image and lanes the reference's word for word "
                   "(%s)\n",
                   gpu.kernel());
        }
        if (kind == 1)
            printf("  memtiers on the GPU: the same with FXC's kernel (%s)\n", gpu.kernel());
    }
}

// --- getting into fast memory --------------------------------------------------------

// ALLOC's resource 7 claims fast memory only, the nearest room there, and
// fails once there is no room of that size; resource 0 lands anywhere.
void test_fast_alloc() {
    MFix f;
    CHECK(f.init());
    TextBuf prog;
    repeat(&prog, "ALLOC", 10);
    const u32 b = plant(f.s, 600, prog.c_str(), 1000);
    CHECK(b != kNone);
    Cpu& c = f.cpu(b);
    c.bx = 7;
    c.cx = 16;
    bool fast = true;
    u32 got = 0;
    for (u32 i = 0; i < 9; ++i) {
        f.s.run_cpu(f.cpu_of(b), 1);
        const Cpu& k = f.s.cpu(f.cpu_of(b));
        if (k.err == 0) {
            ++got;
            fast = fast && k.wh + 16 <= 128 && k.wh == 112 - 16 * i; // down from the boundary, nearest first
        }
    }
    CHECK(got == 8 && fast && f.s.tally().fast_allocs == 8 && f.s.tally().fast_alloc_fails == 1);
    CHECK(f.s.cpu(f.cpu_of(b)).err == 1 && f.s.fast_held() == 128); // full: the ninth fails
    // Plain memory lands beside it, in slow memory.
    f.cpu(b).bx = 0;
    f.s.run_cpu(f.cpu_of(b), 1);
    CHECK(f.s.cpu(f.cpu_of(b)).err == 0 && f.s.cpu(f.cpu_of(b)).wh == 610);
    // A claim bigger than fast memory fails at once.
    f.cpu(b).bx = 7;
    f.cpu(b).cx = 200;
    f.s.run_cpu(f.cpu_of(b), 1);
    CHECK(f.s.cpu(f.cpu_of(b)).err == 1 && f.s.tally().fast_alloc_fails == 2);
    // RELEASE 7 gives up the end of the latest block, as RELEASE 0.
    MFix r;
    CHECK(r.init());
    const u32 rb = plant(r.s, 40, "ALLOC RELEASE NOP0", 1000);
    r.cpu(rb).bx = 7;
    r.cpu(rb).cx = 8;
    r.s.run_cpu(r.cpu_of(rb), 1);
    const u32 at = r.s.cpu(r.cpu_of(rb)).wh; // nearest: right after it, still fast
    CHECK(r.s.cpu(r.cpu_of(rb)).err == 0 && at == 43 && r.s.body(rb).mem == 11);
    r.s.run_cpu(r.cpu_of(rb), 1);
    CHECK(r.s.cpu(r.cpu_of(rb)).err == 0 && r.s.body(rb).mem == 3 && r.s.owner_body(at) == kNone);
    CHECK(soup_ok(f.s) && soup_ok(r.s));
    // Without tiers resource 7 is past ALLOC's map: it fails, counting nothing.
    MFix o;
    CHECK(o.init("mem.fast_share = 0\n"));
    const u32 ob = plant(o.s, 600, "ALLOC NOP0", 1000);
    o.cpu(ob).bx = 7;
    o.cpu(ob).cx = 16;
    o.s.run_cpu(o.cpu_of(ob), 1);
    CHECK(o.s.cpu(o.cpu_of(ob)).err == 1 && o.s.tally().fast_allocs == 0 && o.s.tally().fast_alloc_fails == 0);
    CHECK(o.p.sel.res_n == 4);
}

// --- volatility -------------------------------------------------------------------------

// Pages whose codons changed between two copies of memory, in fast and slow.
void changed_pages(const u8* before, const Soup& s, u32 fast_pages, u64* fast, u64* slow) {
    const u32 pages = s.size() / kPage;
    for (u32 pg = 0; pg < pages; ++pg) {
        if (memcmp(before + pg * kPage, s.mem() + pg * kPage, kPage) == 0)
            continue;
        ++(pg < fast_pages ? *fast : *slow);
    }
}

// A fast page is wiped about mem.fast_volatility (4) times as often as a
// slow one. At mem.fast_volatility 1 the wipes are exactly those without tiers.
void test_volatility() {
    const char* base = "mem.volatility = 0.01\nmem.wipe_pages = 1\nspark.rate = 0\n";
    u64 fast = 0, slow = 0;
    {
        TextBuf cfg;
        cfg.append(base);
        cfg.append("mem.fast_share = 0.25\n");
        MFix f;
        CHECK(f.init(cfg.c_str(), 4096));
        CHECK(f.p.fast_end == 1024); // 16 fast pages of 64
        Array<u8> before;
        for (u64 t = 0; t < 2000; ++t) {
            before.assign(f.s.mem(), f.s.size());
            f.s.tick(t);
            changed_pages(before.data(), f.s, 16, &fast, &slow);
        }
    }
    const double per_fast = static_cast<double>(fast) / 16, per_slow = static_cast<double>(slow) / 48;
    const double ratio = per_fast / per_slow;
    printf("  2,000 ticks at mem.volatility 0.01: %llu fast-page wipes (%.1f a page), %llu slow (%.1f a page): "
           "%.2fx\n",
           static_cast<unsigned long long>(fast), per_fast, static_cast<unsigned long long>(slow), per_slow, ratio);
    CHECK(ratio > 3.3 && ratio < 4.7); // 4, within about four standard deviations
    CHECK(fast > 1000 && slow > 700);
    // At fast_volatility 1 the tiers' wipes are the old process's, draw for draw.
    TextBuf one, none;
    one.append(base);
    one.append("mem.fast_share = 0.25\nmem.fast_volatility = 1\n");
    none.append(base);
    none.append("mem.fast_share = 0\n");
    MFix a, b;
    CHECK(a.init(one.c_str(), 4096) && b.init(none.c_str(), 4096));
    u64 wa = 0, wb = 0; // (the tally is a tick's)
    for (u64 t = 0; t < 300; ++t) {
        a.s.tick(t);
        b.s.tick(t);
        wa += a.s.tally().wipes;
        wb += b.s.tally().wipes;
    }
    CHECK(wa > 100 && wa == wb);
    CHECK(memcmp(a.s.mem(), b.s.mem(), a.s.size()) == 0);
}

// --- worlds ---------------------------------------------------------------------------

// Every other suite runs with mem.fast_share = 0, so their pinned hashes are
// the tiers off. Here: written outright, 0 hashes as the fallback's world;
// tiers on change it (a timed warp world, so the CPU's and the warps' prices
// both matter).
const char* timed_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append("seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 12\nworld.soup_size = 2048\n"
             "world.millicores = 300\nworld.gpus = 300\ngpu.warps = true\nedge.bandwidth = 8\n"
             "cpu.instr_per_core = 25000\ncpu.wear = 0.002\ngpu.wear = 0.002\nspark.timing = last\n"
             "spark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.gpu_grant = 6\nspark.min_len = 8\n"
             "spark.max_len = 32\nhazard.copy = 0.005\nhazard.flaw = 0.001\nhazard.rot = 0.00005\n"
             "salvage.ratio = 4\nworld.disk = 512\nmem.volatility = 0.0005\nseed.at = 600\nseed.form = gpu\n"
             "seed.millicores = 0\nseed.gpus = 64\n"
             "seed.genome = SENSE ALLOC SHR SHR SHR SHR SHR NOP1 COPY DJNZ NOP0 ZERO INC INC ROTB "
             "ZERO INC SHL SHL SHL ALLOC ZERO ROTB INC SHL SHL SHL SHL SHL DIVIDE ZERO ZERO\n"
             "cost.table = home_v1\nwarp.device = cpu\n");
    return t.c_str();
}

void test_off_is_legacy() {
    u64 h[3] = {};
    const char* extra[3] = {"", "mem.fast_share = 0\n", "mem.fast_share = 0.125\n"};
    u64 fast_held = 0;
    for (u32 i = 0; i < 3; ++i) {
        TextBuf cfg;
        cfg.append(timed_world());
        cfg.append(extra[i]);
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        run.run_until(80);
        h[i] = run.world().hash();
        if (i == 2)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                fast_held += run.world().node_soup(n).fast_held();
    }
    CHECK_EQ_U64(h[1], h[0]);
    CHECK(h[2] != h[0] && fast_held > 0);
    // The procedural Phase 4 world likewise (classic costs: the tiers there
    // are the selector and the wipes).
    TextBuf p4, p4off;
    p4.append(kSoupWorldProc);
    p4.append(kPhase4All);
    p4off.append(p4.view());
    p4off.append("mem.fast_share = 0\n");
    Run a, b;
    CHECK(make_run(a, p4.c_str(), 4) && make_run(b, p4off.c_str(), 4));
    a.run_until(100);
    b.run_until(100);
    CHECK_EQ_U64(b.world().hash(), a.world().hash());
}


// --- everything on ----------------------------------------------------------------------

// A busy landscape on the engine's own defaults (no test fallback): timed
// costs, warps, threads and E cores, the GPU and NPU, substrates and sparks
// by activity, the disk and its archive, every disaster frequent, dormancy,
// the lifecycle, and memory tiers (an eighth of each node fast, wiped 4x as
// often, here at a volatility high enough to see). The accelerators on the
// CPU reference, the warps where `device` says.
const char* everything(const char* device) {
    static TextBuf t;
    t.clear();
    t.append("seed = 5\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 24\nland.scale = 4\n"
             "land.resolution = 64\nbridge.max_length = 0.6\nworld.soup_size = 4096\ncpu.instr_per_core = 25000\n"
             "spark.initial = 20\nspark.rate = 0.25\nnode.endurance = 1000\nnode.fallow_min = 20\n"
             "node.recover_every = 10\naccel.gpu = cpu\naccel.npu = cpu\nmem.volatility = 0.0005\n"
             "mem.wipe_warning = 20\n" // (warned wipes, on by default since 2026-11-08, land within the run)
             "disaster.surge_every = 150\ndisaster.crash_every = 40\ndisaster.burnout_every = 30\n"
             "disaster.diskfail_every = 40\ndisaster.storm_every = 100\ndisaster.false_alarms = 0.3\n"
             "disaster.surge_lead_min = 20\ndisaster.surge_lead_max = 60\ndisaster.crash_lead_min = 20\n"
             "disaster.crash_lead_max = 60\ndisaster.burnout_lead_min = 20\ndisaster.burnout_lead_max = 60\n"
             "disaster.diskfail_lead_min = 20\ndisaster.diskfail_lead_max = 60\ndisaster.reboot_min = 20\n"
             "disaster.reboot_max = 50\ndisaster.repair_min = 60\ndisaster.repair_max = 200\n"
             "disaster.storm_life_min = 100\ndisaster.storm_life_max = 300\ndisaster.storm_move_min = 20\n"
             "disaster.storm_move_max = 60\ndisaster.strike_every_min = 10\ndisaster.strike_every_max = 30\n"
             "warp.device = ");
    t.append(device);
    t.append_char('\n');
    return t.c_str();
}

void test_everything() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    const u64 ticks = 160;
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    u64 held_max = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, everything("cpu"), threads[v]));
        const World& w = run.world();
        if (v == 0) {
            bool tiers = true;
            for (u32 n = 0; n < w.node_count(); ++n)
                tiers = tiers && w.node_params(n).fast_end > 0 && w.node_params(n).fast_end % kPage == 0;
            CHECK(tiers && w.substrates() && w.disasters() && w.node_params(0).archive > 0 &&
                  w.node_params(0).cost.timed && w.node_params(0).warps);
        }
        if (v == 3) {
            const u32 n = w.node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            Rng r(99, 0, 0);
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 x = order[i];
                order[i] = order[j];
                order[j] = x;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v != 0)
                continue;
            const u64* s = reinterpret_cast<const u64*>(&w.last_soup_tick());
            u64* d = reinterpret_cast<u64*>(&sum);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                d[i] += s[i];
            u64 held = 0;
            for (u32 n = 0; n < w.node_count(); ++n)
                held += w.node_soup(n).fast_held();
            held_max = held > held_max ? held : held_max;
        }
        hashes[v] = w.hash();
        if (v == 0)
            for (u32 n = 0; n < w.node_count(); ++n)
                CHECK(soup_ok(w.node_soup(n)));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    CHECK(held_max > 0 && sum.wipes > 0 && sum.warp_steps > 0 && (sum.crashes + sum.burnouts + sum.disk_failures) > 0);
    // (The fast-only ALLOC is tested directly above. Random code here finds it
    // about once in 400 ticks since docs/bites-plan.md -- bleeding shortens
    // sparks -- so this short run no longer counts on seeing it.)
    {
        Run a;
        CHECK(make_run(a, everything("cpu"), 4));
        a.run_until(80);
        ByteWriter snap;
        a.snapshot(snap);
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 20, &err));
        b.run_until(ticks);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  everything on with memory tiers: %llu ticks, fast memory held up to %llu codons, %llu fast ALLOCs "
           "(%llu failed), %llu wipes, %llu warp steps, %llu crashes, %llu burnouts, %llu disk failures; the same at "
           "1, 4 and 20 threads, shuffled and through a snapshot; hash %016llx\n",
           static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(held_max),
           static_cast<unsigned long long>(sum.fast_allocs), static_cast<unsigned long long>(sum.fast_alloc_fails),
           static_cast<unsigned long long>(sum.wipes), static_cast<unsigned long long>(sum.warp_steps),
           static_cast<unsigned long long>(sum.crashes), static_cast<unsigned long long>(sum.burnouts),
           static_cast<unsigned long long>(sum.disk_failures), static_cast<unsigned long long>(hashes[0]));
    // On the real GPU, every round checked against the reference: the same world.
    AccelDevice devs[16];
    const u32 nd = accel_list(devs, 16);
    bool gpu = false;
    for (u32 i = 0; i < nd; ++i)
        gpu = gpu || (devs[i].gpu && devs[i].hardware);
    if (!gpu) {
        printf("  everything on: no hardware GPU, the device check skipped\n");
    } else {
        TextBuf cfg;
        cfg.append(everything("gpu"));
        cfg.append("warp.verify = 1\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        run.run_until(ticks);
        const World& w = run.world();
        printf("  everything on with memory tiers, GPU: %s; %llu rounds on the GPU, %llu verified, %llu differed\n",
               w.warp_device_name(), static_cast<unsigned long long>(w.warp_gpu_rounds()),
               static_cast<unsigned long long>(w.warp_verified()), static_cast<unsigned long long>(w.warp_mismatches()));
        CHECK(w.warp_gpu_rounds() > 0 && w.warp_verified() == w.warp_gpu_rounds() && w.warp_mismatches() == 0);
        CHECK_EQ_U64(w.hash(), hashes[0]);
    }
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

} // namespace

void suite_memtiers() {
    test_keys();
    test_cpu_copy();
    test_speeds();
    test_warp_copy();
    test_warp_device();
    test_fast_alloc();
    test_volatility();
    test_off_is_legacy();
    test_everything();
}
