// A varied world (docs/world-plan.md W1, W2, W5): substrates on the map (exact
// level shares, smooth across neighbours, the same map for the same seed, a
// recovered site taking its site's substrate), what each multiplier does (P
// and E cores, GPU throughput and the warps' clock, NPU multiply-adds, heat
// unchanged), sparks following node activity, the per-node file, and a busy
// landscape with everything on, the same at 1, 4 and 20 threads, under
// shuffled dispatch, through a snapshot and on the GPU.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/accel.h"
#include "nova/hash.h"
#include "nova/keys.h"
#include "nova/landscape.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

// --- the map ---------------------------------------------------------------------

void random_points(u32 n, u64 seed, double width, Array<double>* x, Array<double>* y) {
    Rng r(seed, 0, 0);
    x->clear();
    y->clear();
    for (u32 i = 0; i < n; ++i) {
        x->push(r.unit() * width);
        y->push(r.unit());
    }
}

// The shares are exact: 10/20/40/20/10 of the nodes, rounded so they add up.
void test_shares() {
    SubstrateParams s;
    u32 c[kSubstrateLevels];
    substrate_counts(s, 1000, c);
    CHECK(c[0] == 100 && c[1] == 200 && c[2] == 400 && c[3] == 200 && c[4] == 100);
    substrate_counts(s, 24, c); // 2.4 4.8 9.6 4.8 2.4: the largest remainders round up
    CHECK(c[0] == 2 && c[1] == 5 && c[2] == 10 && c[3] == 5 && c[4] == 2);
    substrate_counts(s, 1, c);
    CHECK(c[kSubstrateOne] == 1);
    bool sums = true;
    for (u32 n = 1; n < 400; ++n) {
        substrate_counts(s, n, c);
        sums = sums && c[0] + c[1] + c[2] + c[3] + c[4] == n;
    }
    CHECK(sums);
    SubstrateParams odd;
    const double shares[kSubstrateLevels] = {0, 1, 1, 0, 2}; // relative: a quarter, a quarter, a half
    for (u32 l = 0; l < kSubstrateLevels; ++l)
        odd.share[l] = shares[l];
    substrate_counts(odd, 100, c);
    CHECK(c[0] == 0 && c[1] == 25 && c[2] == 25 && c[3] == 0 && c[4] == 50);
    CHECK(substrate_multiplier(0, 0.1) == 0.1 && substrate_multiplier(1, 0.1) == 0.5 &&
          substrate_multiplier(2, 0.1) == 1.0 && substrate_multiplier(3, 0.1) == 1.5 &&
          substrate_multiplier(4, 0.1) == 2.0);

    // Every type's levels on 1,000 nodes take exactly those shares, static
    // or drifting; the same seed gives the same map, another seed doesn't.
    Array<double> x, y;
    random_points(1000, 3, 1.6, &x, &y);
    Array<u8> a, b, other, drift;
    substrate_levels(42, s, x, y, -1.0, &a);
    substrate_levels(42, s, x, y, -1.0, &b);
    substrate_levels(43, s, x, y, -1.0, &other);
    substrate_levels(42, s, x, y, 2.5, &drift);
    CHECK(a.size() == 1000u * kSubstrateTypes && a.size() == b.size());
    CHECK(memcmp(a.data(), b.data(), a.size()) == 0);
    CHECK(memcmp(a.data(), other.data(), a.size()) != 0);
    CHECK(memcmp(a.data(), drift.data(), a.size()) != 0);
    bool exact = true, types_differ = false;
    for (u32 t = 0; t < kSubstrateTypes; ++t) {
        u32 at[kSubstrateLevels] = {}, dr[kSubstrateLevels] = {};
        for (u32 i = 0; i < 1000; ++i) {
            ++at[a[i * kSubstrateTypes + t]];
            ++dr[drift[i * kSubstrateTypes + t]];
            types_differ = types_differ || (t > 0 && a[i * kSubstrateTypes + t] != a[i * kSubstrateTypes]);
        }
        for (u32 l = 0; l < kSubstrateLevels; ++l)
            exact = exact && at[l] == (l == 2 ? 400u : (l == 0 || l == 4 ? 100u : 200u)) && dr[l] == at[l];
    }
    CHECK(exact);
    CHECK(types_differ); // each type its own field
}

// Neighbours (paths on a 1,000-node landscape) differ far less than random
// pairs: smooth fields, so crossing from one region to the next usually
// changes a type by a step.
void test_neighbours() {
    LandParams lp;
    lp.nodes = 1000;
    Landscape L;
    TextBuf err;
    CHECK(generate_landscape(5, lp, &L, &err));
    // The defaults (smooth 1, scale 1.5 since the integration: the registry's
    // and SubstrateParams' own), then for the record the approved first
    // defaults (0.7 at scale 3, a patchy map) and the steps between.
    SubstrateParams def;
    const Key* ks = key_find("substrate.smooth");
    const Key* kc = key_find("substrate.scale");
    CHECK(def.smooth == 1.0 && def.scale == 1.5 && ks && kc && ks->def == def.smooth && kc->def == def.scale);
    const double smooths[5] = {1.0, 0.7, 0.9, 1.0, 0.7}, scales[5] = {1.5, 3, 3, 3, 1.5};
    double ratio[5] = {}, types[5] = {};
    for (u32 v = 0; v < 5; ++v) {
        SubstrateParams s;
        s.smooth = smooths[v];
        s.scale = scales[v];
        Array<u8> lv;
        substrate_levels(5, s, L.x, L.y, -1.0, &lv);
        auto diff = [&](u32 a, u32 b, double* steps, double* types) {
            for (u32 t = 0; t < kSubstrateTypes; ++t) {
                const i32 d = static_cast<i32>(lv[a * kSubstrateTypes + t]) - lv[b * kSubstrateTypes + t];
                *steps += d < 0 ? -d : d;
                *types += d != 0;
            }
        };
        double nb_steps = 0, nb_types = 0, rd_steps = 0, rd_types = 0;
        u32 nb = 0;
        for (usize e = 0; e < L.keys.size(); ++e) {
            const u32 a = static_cast<u32>(L.keys[e] >> 32), b = static_cast<u32>(L.keys[e]);
            if (L.bridge[e] || a > b)
                continue;
            diff(a, b, &nb_steps, &nb_types);
            ++nb;
        }
        Rng r(77, 0, 0);
        const u32 pairs = 20000;
        for (u32 i = 0; i < pairs; ++i) {
            const u32 a = static_cast<u32>(r.below(1000)), b = static_cast<u32>(r.below(1000));
            diff(a, b, &rd_steps, &rd_types);
        }
        const double per = kSubstrateTypes;
        ratio[v] = (nb_steps / nb) / (rd_steps / pairs);
        types[v] = nb_types / nb;
        printf("  substrates (smooth %.1f, scale %.1f): neighbours differ by %.3f steps a type (%.2f of 8 types), "
               "random pairs by %.3f (%.2f): %.0f%%\n",
               smooths[v], scales[v], nb_steps / nb / per, nb_types / nb, rd_steps / pairs / per, rd_types / pairs,
               100.0 * ratio[v]);
    }
    // The defaults: neighbours share most of their levels (1.75 of 8 types
    // differ, 19% of a random pair's difference when measured), so crossing
    // from one region to the next usually changes a type or two by a step.
    CHECK(ratio[0] < 0.3 && types[0] < 2.5);
    CHECK(ratio[1] < 0.8);  // the first defaults: under random pairs, but patchy (5.2 types)
    CHECK(ratio[3] < 0.35); // following the fields alone at scale 3
    CHECK(ratio[3] < ratio[2] && ratio[2] < ratio[1]); // smoother, smoother still
    CHECK(ratio[0] < ratio[3] && ratio[0] < ratio[4]); // and larger features smoother again
}

// A small landscape world (as the landscape suite's); land_sub() turns
// substrates on in it.
const char* const kLandBase = "seed = 7\n"
                              "world.model = soup\n"
                              "world.graph = landscape\n"
                              "world.nodes = 90\n"
                              "land.scale = 4\n"
                              "land.resolution = 96\n"
                              "paths.hubs = 0.05\n"
                              "paths.isolated = 0.03\n"
                              "bridge.count = 2\n"
                              "bridge.max_length = 0.6\n"
                              "bridge.latency_scale = 10\n"
                              "bridge.open_every = 40\n"
                              "bridge.open_ticks = 15\n"
                              "world.soup_size = 2048\n"
                              "world.millicores = 600\n"
                              "world.gpus = 60\n"
                              "world.npus = 40\n"
                              "res.memory.spread = 0.6\n"
                              "res.cpu.spread = 0.8\n"
                              "spark.spread = 0.7\n"
                              "edge.bandwidth = 8\n"
                              "node.endurance = 4000\n"
                              "node.fallow_min = 20\n"
                              "node.recover_every = 10\n"
                              "cpu.instr_per_core = 2500\n"
                              "cpu.wear = 0.002\n"
                              "spark.timing = last\n"
                              "spark.initial = 20\n"
                              "spark.rate = 0.5\n"
                              "spark.grant = 20\n"
                              "spark.min_len = 8\n"
                              "spark.max_len = 24\n"
                              "seed.genome = ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE\n"
                              "seed.at = 600\n"
                              "seed.millicores = 200\n"
                              "hazard.copy = 0.005\n"
                              "hazard.flaw = 0.001\n"
                              "hazard.rot = 0.00005\n";

const char* land_sub() {
    static TextBuf t;
    if (!t.size()) {
        t.append(kLandBase);
        t.append("substrate.enabled = true\n");
    }
    return t.c_str();
}

bool node_matches(const World& w, u32 n, const Array<u8>& map) {
    const SoupParams& p = w.node_params(n);
    // (the cores' and accelerators' speeds are floats; the tiers', which
    // divide costs, doubles: soup.h)
    const double sp[kSubstrateTypes] = {p.p_speed,        p.e_speed,        p.gpu_speed,  p.npu_speed,
                                        p.mem_fast_speed, p.mem_slow_speed, p.disk_speed, p.archive_speed};
    bool ok = true;
    for (u32 t = 0; t < kSubstrateTypes; ++t) {
        const u8 l = map[static_cast<usize>(n) * kSubstrateTypes + t];
        const double m = substrate_multiplier(l, w.substrate_params().floor_mult);
        ok = ok && w.substrate_level(n, t) == l && sp[t] == (t < kSubMemFast ? static_cast<float>(m) : m);
    }
    return ok;
}

// The world's map is the generator's for its seed and positions; each node's
// speeds are its levels' multipliers. A landscape's substrates are its sites'
// own, so a recovered site takes its site's substrate again. A ring has none.
void test_world_map() {
    Run run;
    CHECK(make_run(run, land_sub(), 4));
    const World& w = run.world();
    CHECK(w.substrates());
    Array<double> x, y;
    for (u32 n = 0; n < w.node_count(); ++n) {
        x.push(w.node_x(n));
        y.push(w.node_y(n));
    }
    Array<u8> map;
    substrate_levels(7, w.substrate_params(), x, y, -1.0, &map);
    bool all = true;
    for (u32 n = 0; n < w.node_count(); ++n)
        all = all && node_matches(w, n, map);
    CHECK(all);
    run.run_until(300);
    bool after = true;
    u32 later = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        after = after && node_matches(w, n, map);
        later += w.life(n) > 0;
    }
    printf("  substrates: landscape, 300 ticks, %llu recoveries (%u nodes in a later life), every node on its "
           "site's substrate: %s\n",
           static_cast<unsigned long long>(w.recoveries_total()), later, after ? "yes" : "NO");
    CHECK(after && later > 0);

    // The same seed, the same map; off, every speed 1.
    Run again;
    CHECK(make_run(again, land_sub(), 1));
    bool same = true;
    for (u32 n = 0; n < again.world().node_count(); ++n)
        same = same && node_matches(again.world(), n, map);
    CHECK(same);
    Run off; // the legacy fallback: off, every speed 1
    CHECK(make_run(off, kLandBase, 1));
    CHECK(!off.world().substrates() && off.world().substrate_mult(3, kSubNpu) == 1.0);
    CHECK(off.world().node_params(3).gpu_speed == 1.0f && off.world().node_params(3).spark_activity == 0.0);
    Run ring; // a ring has no map: on by default, yet every level 1x
    CHECK(Config::set_fallback(nullptr, nullptr));
    CHECK(make_run(ring,
                   "world.model = soup\nworld.nodes = 4\nworld.soup_size = 1024\naccel.gpu = cpu\naccel.npu = cpu\n"
                   "warp.device = cpu\n",
                   1));
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    CHECK(!ring.world().substrates() && ring.world().substrate_level(2, kSubGpu) == kSubstrateOne);
    CHECK(ring.world().node_params(2).p_speed == 1.0f && ring.world().node_params(2).npu_speed == 1.0f);
    CHECK(ring.world().node_params(2).spark_activity == 2.0); // (sparks by activity need no map)
    // ... and the five shares can't all be 0.
    TextBuf zero;
    zero.append(land_sub());
    zero.append("substrate.share_floor = 0\nsubstrate.share_half = 0\nsubstrate.share_one = 0\n"
                "substrate.share_one_half = 0\nsubstrate.share_two = 0\n");
    Run z;
    Config::ParseError pe;
    CHECK(z.config().parse(zero.view(), &pe));
    TextBuf err;
    CHECK(!z.init(1, &err));
}

// Where the world drifts (a procedural world with world.drift), a recovering
// site takes the substrate the fields give its site then; a snapshot taken
// through it resumes exactly.
const char* const kDriftSub = "seed = 9\n"
                              "world.model = soup\n"
                              "world.graph = procedural\n"
                              "world.nodes = 40\n"
                              "world.soup_size = 1024\n"
                              "world.millicores = 400\n"
                              "cpu.instr_per_core = 2500\n"
                              "spark.initial = 10\n"
                              "spark.rate = 0.5\n"
                              "spark.grant = 20\n"
                              "spark.min_len = 8\n"
                              "spark.max_len = 24\n"
                              "seed.genome = ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE\n"
                              "seed.at = 300\n"
                              "seed.millicores = 200\n"
                              "node.endurance = 1500\n"
                              "node.fallow_min = 10\n"
                              "node.recover_every = 5\n"
                              "world.land = 0.8\n"
                              "world.drift = 0.01\n"
                              "substrate.enabled = true\n"
                              "spark.activity = 2\n";

void test_drift_recovery() {
    Run run;
    CHECK(make_run(run, kDriftSub, 4));
    run.run_until(300);
    const World& w = run.world();
    Array<double> x, y;
    for (u32 n = 0; n < w.node_count(); ++n) {
        x.push(w.node_x(n));
        y.push(w.node_y(n));
    }
    Array<u8> first;
    substrate_levels(9, w.substrate_params(), x, y, 0.0, &first);
    bool all = true;
    u32 later = 0, moved = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        Array<u8> then;
        substrate_levels(9, w.substrate_params(), x, y, static_cast<double>(w.born(n)) * 0.01, &then);
        all = all && node_matches(w, n, then);
        if (w.life(n) > 0) {
            ++later;
            moved += memcmp(then.data() + n * kSubstrateTypes, first.data() + n * kSubstrateTypes,
                            kSubstrateTypes) != 0;
        }
    }
    printf("  substrates: drifting world, 300 ticks, %u nodes in a later life, %u of them on another substrate "
           "than at the start\n",
           later, moved);
    CHECK(all && later > 0 && moved > 0);
    const u64 want = w.hash();
    Run a;
    CHECK(make_run(a, kDriftSub, 4));
    a.run_until(150);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 20, &err));
    b.run_until(300);
    CHECK_EQ_U64(b.world().hash(), want);
}

// --- what a multiplier does (W1) ----------------------------------------------------

struct CFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 1024) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
};

// Instructions a body of `units` P millicores runs in 10 ticks at P speed `ps`.
u64 p_run(float ps, const char* extra, u32 units) {
    CFix f;
    if (!f.init(extra))
        return 0;
    f.p.p_speed = ps;
    const u32 b = plant(f.s, 100, "INC INC INC INC", units);
    if (b == kNone)
        return 0;
    for (u64 t = 0; t < 10; ++t)
        f.s.tick(t);
    return f.s.cpu(f.s.body(b).cpus).executed;
}

// Instructions a body all of E millicores runs in 10 ticks at E speed `es`
// (home_v1: an E-core add is 218 ps, a tick 100 P adds).
u64 e_run(float es) {
    CFix f;
    if (!f.init("world.millicores = 2000\ncost.table = home_v1\ncpu.e_share = 1\ncpu.instr_per_core = 100\n"))
        return 0;
    f.p.e_speed = es;
    const u32 b = plant(f.s, 100, "INC INC INC INC", 1000);
    if (b == kNone || f.s.e_units(b) != 1000)
        return 0;
    for (u64 t = 0; t < 10; ++t)
        f.s.tick(t);
    return f.s.cpu(f.s.body(b).cpus).executed;
}

bool within(u64 v, u64 want, u64 slack) { return v + slack >= want && v <= want + slack; }

void test_cores() {
    // P cores: 1,000 millicores at 100 a tick; at 2x twice that, at the floor a tenth.
    const char* p = "world.millicores = 1000\ncpu.instr_per_core = 100\n";
    const u64 p1 = p_run(1.0f, p, 1000), p2 = p_run(2.0f, p, 1000), pf = p_run(0.1f, p, 1000);
    CHECK(p1 == 1000 && p2 == 2000 && pf == 100);
    // With threads a thread runs on one core: a core's tick, at the core's speed.
    const char* t = "world.millicores = 2000\ncpu.instr_per_core = 100\ncpu.threads = true\n";
    const u64 t1 = p_run(1.0f, t, 2000), t2 = p_run(2.0f, t, 2000), tf = p_run(0.1f, t, 2000);
    CHECK(t1 == 1000 && t2 == 2000 && tf == 100);
    // E cores (87.2 adds a tick at 1x, with the carry over 10 ticks).
    const u64 e1 = e_run(1.0f), e2 = e_run(2.0f), ef = e_run(0.1f);
    CHECK(within(e1, 872, 1) && within(e2, 2 * e1, 2) && within(ef * 10, e1, 10));
    printf("  substrates: P core 10 ticks: %llu, %llu at 2x, %llu at the floor; a thread %llu, %llu, %llu; "
           "E core %llu, %llu, %llu\n",
           static_cast<unsigned long long>(p1), static_cast<unsigned long long>(p2),
           static_cast<unsigned long long>(pf), static_cast<unsigned long long>(t1),
           static_cast<unsigned long long>(t2), static_cast<unsigned long long>(tf),
           static_cast<unsigned long long>(e1), static_cast<unsigned long long>(e2),
           static_cast<unsigned long long>(ef));

    // A thread on half a P core and a whole E core twice as fast: half a tick
    // on its P core, then the rest of its core-tick on E, where it runs twice
    // as much: 1.5 ticks' time, a tick of it on E.
    CFix m;
    CHECK(m.init("world.millicores = 2000\ncost.table = home_v1\ncpu.e_share = 0.5\ncpu.threads = true\n"
                 "cpu.instr_per_core = 25000\n"));
    m.p.e_speed = 2.0f;
    const u32 b = plant(m.s, 100, "INC INC INC INC", 0);
    CHECK(b != kNone && m.s.claim_p_units(b, 500) == 500 && m.s.claim_e_units(b, 1000) == 1000);
    m.s.tick(0);
    const u32 c = m.s.body(b).cpus;
    const double tick = 25000.0 * 190.0;
    const double got = m.s.cpu_time(c), on_e = m.s.cpu_etime(c);
    CHECK(fabs(got - 1.5 * tick) < 1000 && fabs(on_e - tick) < 1000);

    // Heat follows how busy a unit is, not how much it does: flat out, a P
    // unit takes cpu_gain (6) at 1x, 2x and the floor alike.
    const float speeds[3] = {1.0f, 2.0f, 0.1f};
    for (float sp : speeds) {
        SoupParams hp;
        CHECK(make_params(&hp, "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
                               "world.millicores = 1000\ncpu.instr_per_core = 100\n",
                          1024));
        hp.p_speed = sp;
        Soup s;
        s.init(&hp, 23, 0);
        const u32 hb = plant(s, 100, "INC INC INC INC", 1000);
        s.tick(0);
        float h = -1;
        for (u32 u = 0; u < 1000; ++u)
            if (s.unit_owner(u) == hb)
                h = s.unit_heat(u);
        CHECK(h > 5.999f && h < 6.001f);
    }
}

// A body born on the GPU running INCs, holding `gpus` units.
u32 plant_gpu(CFix& f, u32 gpus) {
    Array<u8> code;
    TextBuf err;
    if (!genome_from_names(f.p.table, "INC INC INC INC", &code, &err))
        return kNone;
    for (usize i = 0; i < code.size(); ++i)
        f.s.mem_mut()[100 + i] = code[i];
    return f.s.create_body_form(100, static_cast<u32>(code.size()), 0, gpus, kFormGpu);
}

// Warp steps in 10 ticks at GPU speed gs. classic: throughput (8 units x 256
// of work, 16 a step: 128 steps a tick); timed: each warp's clock (a tick of
// 1,393 P adds is 190 of a lane's adds; 64 units, so the clock binds).
u64 warp_steps(float gs, bool timed) {
    CFix f;
    if (!f.init(timed ? "world.millicores = 2000\nworld.gpus = 400\ngpu.warps = true\ncost.table = home_v1\n"
                        "cpu.instr_per_core = 1393\n"
                      : "world.millicores = 2000\nworld.gpus = 400\ngpu.warps = true\nwarp.lane_cost = 0\n"))
        return 0;
    f.p.gpu_speed = gs;
    const u32 b = plant_gpu(f, timed ? 64 : 8);
    if (b == kNone || f.s.warp_count(b) != 1)
        return 0;
    const u32 w = f.s.warp_head(b);
    for (u64 t = 0; t < 10; ++t)
        f.s.tick(t);
    return f.s.warp(w).steps;
}

// Timed block ops (home_v1, instant mode): an op longer than a tick is an
// action lasting the device's start-up plus its work over the body's units, at
// the substrate's speed. Its end tick (from tick 0), for one GPU compare of a
// million elements or one NPU matmul of 32 (32,768 multiply-adds), on 1 unit.
u64 device_end(bool npu, float speed) {
    CFix f;
    if (!f.init(npu ? "world.millicores = 1000\nworld.npus = 100\ncost.table = home_v1\ncpu.instr_per_core = 25000\n"
                    : "world.millicores = 1000\nworld.gpus = 100\ncost.table = home_v1\ncpu.instr_per_core = 25000\n"))
        return 0;
    if (npu)
        f.p.npu_speed = speed;
    else
        f.p.gpu_speed = speed;
    const u32 b = plant(f.s, 100, "GPU", 500);
    if (b == kNone || (npu ? f.s.claim_npus(b, 1) : f.s.claim_gpus(b, 1)) != 1)
        return 0;
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.bx = npu ? 18 : 0;
    c.cx = npu ? 32 : 1000000;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    return f.s.actions().size() == 1 ? f.s.actions()[0].end : 0;
}

void test_device_time() {
    CostTable h;
    CHECK(builtin_costs("home_v1", &h));
    const double tick = 25000.0 * 190.0;
    auto want = [&](double start, double work, double speed) {
        return static_cast<u64>(ceil((start + floor(work / speed)) / tick));
    };
    const double gwork = 1e6 * static_cast<double>(h.gpu_thr), nwork = 32768.0 * h.npu_mac * 1000.0;
    const u64 g1 = device_end(false, 1.0f), g2 = device_end(false, 2.0f);
    const u64 n1 = device_end(true, 1.0f), n2 = device_end(true, 2.0f);
    CHECK(g1 == want(static_cast<double>(h.gpu_start), gwork, 1.0) && g2 == want(static_cast<double>(h.gpu_start), gwork, 2.0));
    CHECK(n1 == want(static_cast<double>(h.npu_start), nwork, 1.0) && n2 == want(static_cast<double>(h.npu_start), nwork, 2.0));
    CHECK(g2 < g1 && n2 < n1);
    printf("  substrates: timed block ops end at tick %llu (GPU) and %llu (NPU), at 2x %llu and %llu\n",
           static_cast<unsigned long long>(g1), static_cast<unsigned long long>(n1),
           static_cast<unsigned long long>(g2), static_cast<unsigned long long>(n2));
}

void test_gpu() {
    // Block ops: a unit's element-ops a tick (classic: 32).
    float cap[3] = {};
    const float speeds[3] = {1.0f, 2.0f, 0.1f};
    for (u32 i = 0; i < 3; ++i) {
        CFix f;
        CHECK(f.init("world.millicores = 1000\nworld.gpus = 100\n"));
        f.p.gpu_speed = speeds[i];
        const u32 b = plant(f.s, 100, "GPU", 500);
        CHECK(f.s.claim_gpus(b, 8) == 8);
        f.s.prepare_gpu();
        cap[i] = f.s.gpu_capacity(b);
    }
    CHECK(cap[0] == 256.0f && cap[1] == 512.0f && fabs(cap[2] - 25.6f) < 1e-3f);
    // Warps: lane throughput, and each warp's own clock.
    const u64 c1 = warp_steps(1.0f, false), c2 = warp_steps(2.0f, false), cf = warp_steps(0.1f, false);
    CHECK(c1 == 1280 && c2 == 2560 && within(cf, 128, 1));
    const u64 k1 = warp_steps(1.0f, true), k2 = warp_steps(2.0f, true), kf = warp_steps(0.1f, true);
    CHECK(k1 == 1900 && k2 == 3800 && kf == 190);
    printf("  substrates: GPU unit %.1f element-ops, %.1f at 2x, %.1f at the floor; warp steps in 10 ticks by "
           "throughput %llu, %llu, %llu, by its clock %llu, %llu, %llu\n",
           cap[0], cap[1], cap[2], static_cast<unsigned long long>(c1), static_cast<unsigned long long>(c2),
           static_cast<unsigned long long>(cf), static_cast<unsigned long long>(k1),
           static_cast<unsigned long long>(k2), static_cast<unsigned long long>(kf));
}

void test_npu() {
    // A unit's multiply-adds a tick (classic: npu.macs 256), and a matrix
    // multiply that fits only at the substrate's speed.
    const float speeds[3] = {1.0f, 2.0f, 0.1f};
    float cap[3] = {};
    bool mm9[3] = {}, mm8[3] = {};
    TextBuf prog;
    prog.append("GPU");
    for (u32 i = 0; i < 120; ++i) // its own codons as data (a program reads only its own)
        prog.append(" INC");
    for (u32 i = 0; i < 3; ++i) {
        for (u32 k = 8; k <= 9; ++k) {
            CFix f;
            CHECK(f.init("world.millicores = 1000\nworld.npus = 100\n"));
            f.p.npu_speed = speeds[i];
            const u32 b = plant(f.s, 100, prog.c_str(), 500);
            CHECK(f.s.claim_npus(b, 2) == 2);
            f.s.prepare_gpu();
            cap[i] = f.s.npu_capacity(b);
            Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
            c.ip = 100;
            c.ax = 101;
            c.bx = 18; // matmul: k^3 MACs, 512 (k = 8) or 729 (k = 9), on 2 units of 256
            c.cx = k;
            c.rh = 101;
            c.wh = 101;
            f.s.run_cpu(f.s.body(b).cpus, 1);
            (k == 8 ? mm8 : mm9)[i] = c.err == 0;
        }
    }
    CHECK(cap[0] == 512.0f && cap[1] == 1024.0f && fabs(cap[2] - 51.2f) < 1e-3f);
    CHECK(mm8[0] && !mm9[0] && mm8[1] && mm9[1] && !mm8[2] && !mm9[2]);
    printf("  substrates: NPU unit pair %.1f multiply-adds, %.1f at 2x, %.1f at the floor\n", cap[0], cap[1], cap[2]);
}

// --- sparks follow node activity (W5) ------------------------------------------------

void test_sparks() {
    // The rate's curve: rate x (1 - occupancy)^2.
    CFix f;
    CHECK(f.init("world.millicores = 4000\nspark.rate = 0.8\nspark.activity = 2\nlife.dormancy = true\n", 10000));
    CHECK(f.s.occupancy() == 0.0 && f.s.spark_rate_now() == 0.8); // empty: the full rate
    const u32 half = f.s.create_body(0, 5000, 10);
    CHECK(half != kNone && f.s.occupancy() == 0.5 && fabs(f.s.spark_rate_now() - 0.2) < 1e-12);
    const u32 more = f.s.create_body(5000, 4900, 10); // 99% held by the living
    CHECK(more != kNone && fabs(f.s.occupancy() - 0.99) < 1e-12);
    CHECK(fabs(f.s.spark_rate_now() - 0.8e-4) < 1e-12); // a hundredth of a hundredth
    // A resting dormant program isn't counted (it lands in the last 100), nor is waste.
    Dormant d = {};
    d.millicores = 10;
    const u8 code[16] = {};
    CargoTag tag = {};
    CHECK(f.s.land_dormant(code, 16, d, tag) && f.s.resting_count() == 1);
    CHECK(fabs(f.s.occupancy() - 0.99) < 1e-12 && f.s.waste() == 100 - 16);
    CFix g;
    CHECK(g.init("world.millicores = 4000\nspark.rate = 0.8\nspark.activity = 2\n", 1000));
    CHECK(g.s.create_body(0, 1000, 10) != kNone && g.s.spark_rate_now() == 0.0); // full: none
    CFix off; // spark.activity 0: by time alone, as before
    CHECK(off.init("world.millicores = 4000\nspark.rate = 0.8\nspark.activity = 0\n", 10000));
    CHECK(off.s.create_body(0, 9900, 10) != kNone && off.s.spark_rate_now() == 0.8);

    // In a running soup: an empty node sparks at about the full rate (its
    // sparks barely fill it: with a thousandth of an instruction a tick they
    // claim no memory), a node 99% full almost never.
    u64 counts[2] = {};
    for (u32 v = 0; v < 2; ++v) {
        CFix r;
        CHECK(r.init("world.millicores = 100000\ncpu.instr_per_core = 1\nspark.rate = 0.5\nspark.activity = 2\n"
                     "spark.grant = 1\nspark.min_len = 8\nspark.max_len = 8\n",
                     100000));
        if (v == 1) { // a body of 99,000 NOPs, which runs for ever
            Array<u8> nop;
            TextBuf err;
            CHECK(genome_from_names(r.p.table, "NOP0", &nop, &err));
            for (u32 a = 0; a < 99000; ++a)
                r.s.mem_mut()[a] = nop[0];
            CHECK(r.s.create_body(0, 99000, 1000) != kNone && r.s.occupancy() == 0.99);
        }
        for (u64 t = 0; t < 400; ++t) {
            r.s.tick(t);
            counts[v] += r.s.tally().sparks;
        }
    }
    printf("  substrates: sparks in 400 ticks at rate 0.5: %llu on an empty node, %llu on one 99%% full\n",
           static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(counts[1]));
    CHECK(counts[0] > 150 && counts[0] < 250 && counts[1] <= 2);
}

// --- the per-node file and old snapshots -----------------------------------------------

void test_nodes_file() {
    const char* stats = "nova_test_sub_stats.csv";
    const char* nodes = "nova_test_sub_nodes.csv";
    Run run;
    CHECK(make_run(run, land_sub(), 2));
    TextBuf err;
    CHECK(run.open_stats(stats, 10, &err));
    CHECK(run.open_nodes(nodes, &err));
    run.run_until(20);
    run.finish();
    Array<u8> bytes;
    CHECK(read_file(nodes, &bytes));
    bytes.push(0);
    const char* text = reinterpret_cast<const char*>(bytes.data());
    const char* tail = ",life,born,sub_p_cores,sub_e_cores,sub_gpu,sub_npu,sub_mem_fast,sub_mem_slow,sub_disk_fast,"
                       "sub_disk_slow,occupancy,"
                       "symptom_ticks,struck,cores_offline," // (then the disasters', docs/disasters-plan.md X7)
                       "sleeps,drifts,"                       // (and docs/bites-plan.md section 7's)
                       "warp_lanes\n";                        // (and docs/inject-plan.md W1's)
    const char* nl = strchr(text, '\n');
    CHECK(nl != nullptr && strstr(text, tail) == nl + 1 - strlen(tail));
    // The last window's row for node 5 (the rows go window by window, node by
    // node): its multipliers and occupancy, in columns 29-37.
    const World& w = run.world();
    Array<const char*> lines;
    for (const char* p = text; *p; ++p)
        if (p == text || p[-1] == '\n')
            lines.push(p);
    CHECK(lines.size() == 1 + 2 * w.node_count());
    const char* row = lines[lines.size() - w.node_count() + 5];
    double v[45] = {};
    u32 col = 0;
    for (const char* p = row; *p && *p != '\n' && col < 45; ++col) {
        const char* e = p;
        while (*e && *e != ',' && *e != '\n')
            ++e;
        parse_f64(Str(p, static_cast<usize>(e - p)), &v[col]);
        p = *e == ',' ? e + 1 : e;
    }
    bool ok = col == 44 && v[2] == 5 && v[43] == w.node_params(5).warp_lanes; // (the last: its warp width)
    for (u32 t = 0; t < kSubstrateTypes; ++t)
        ok = ok && fabs(v[29 + t] - w.substrate_mult(5, t)) < 1e-9;
    ok = ok && fabs(v[37] - w.node_soup(5).occupancy()) < 1e-6;
    CHECK(ok);
    remove(stats);
    remove(nodes);
}

// --- everything on ----------------------------------------------------------------------

// A busy landscape on the engine's own defaults (no test fallback): every
// feature on -- substrates and sparks by activity with timed costs, threads
// and E cores, warps, the GPU and NPU, the disk, signals, dormancy, vents --
// with the lifecycle quick enough that sites collapse and recover. The
// accelerators run on the CPU reference, the warps where `device` says.
const char* everything(const char* device) {
    static TextBuf t;
    t.clear();
    t.append("seed = 5\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 24\nland.scale = 4\n"
             "land.resolution = 64\nbridge.max_length = 0.6\nworld.soup_size = 4096\ncpu.instr_per_core = 25000\n"
             "spark.initial = 20\nspark.rate = 0.25\nnode.endurance = 1000\nnode.fallow_min = 20\n"
             "node.recover_every = 10\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = ");
    t.append(device);
    t.append_char('\n');
    return t.c_str();
}

void test_everything() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, everything("cpu"), threads[v]));
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
        run.run_until(160);
        hashes[v] = run.world().hash();
        if (v == 0) {
            const World& w = run.world();
            u64 bodies = 0, warps = 0, sparks = 0;
            double occ = 0;
            for (u32 n = 0; n < w.node_count(); ++n) {
                bodies += w.node_soup(n).body_count();
                warps += w.node_soup(n).warps_alive();
                occ += w.node_soup(n).occupancy();
                CHECK(soup_ok(w.node_soup(n)));
            }
            sparks = w.last_soup_tick().sparks;
            printf("  everything on: 160 ticks, %llu bodies, %llu warps, mean occupancy %.3f, %llu collapses, "
                   "%llu recoveries, hash %016llx\n",
                   static_cast<unsigned long long>(bodies), static_cast<unsigned long long>(warps),
                   occ / w.node_count(), static_cast<unsigned long long>(w.collapses_total()),
                   static_cast<unsigned long long>(w.recoveries_total()), static_cast<unsigned long long>(hashes[0]));
            (void)sparks;
            CHECK(w.substrates() && w.node_params(0).spark_activity == 2.0 && bodies > 0 && warps > 0);
        }
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    {
        Run a;
        CHECK(make_run(a, everything("cpu"), 4));
        a.run_until(80);
        ByteWriter snap;
        a.snapshot(snap);
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 20, &err));
        b.run_until(160);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
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
        run.run_until(160);
        const World& w = run.world();
        printf("  everything on, GPU: %s; %llu rounds on the GPU, %llu verified, %llu differed\n", w.warp_device_name(),
               static_cast<unsigned long long>(w.warp_gpu_rounds()), static_cast<unsigned long long>(w.warp_verified()),
               static_cast<unsigned long long>(w.warp_mismatches()));
        CHECK(w.warp_gpu_rounds() > 0 && w.warp_verified() == w.warp_gpu_rounds() && w.warp_mismatches() == 0);
        CHECK_EQ_U64(w.hash(), hashes[0]);
    }
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

} // namespace

void suite_substrates() {
    test_shares();
    test_neighbours();
    test_world_map();
    test_drift_recovery();
    test_cores();
    test_gpu();
    test_npu();
    test_device_time();
    test_sparks();
    test_nodes_file();
    test_everything();
}
