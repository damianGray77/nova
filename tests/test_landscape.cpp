// Phase 7 (design.md 6.13): landscape generation -- the triangulation, land and
// landmasses, node placement, paths, bridges and distributions.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/graph.h"
#include "nova/hashmap.h"
#include "nova/landscape.h"
#include "nova/rng.h"
#include "nova/snapshot.h"

using namespace nova;

namespace {

double orient(const double* x, const double* y, u32 a, u32 b, u32 c) {
    return (x[b] - x[a]) * (y[c] - y[a]) - (y[b] - y[a]) * (x[c] - x[a]);
}

// Every triangle counter-clockwise, and no point strictly inside any
// triangle's circumcircle (the Delaunay property), checked by brute force.
void test_delaunay() {
    for (u32 trial = 0; trial < 4; ++trial) {
        const u32 n = 50 + 150 * trial;
        Array<double> x, y;
        Rng r(77 + trial, 0, 0);
        for (u32 i = 0; i < n; ++i) {
            x.push(r.unit() * (1 + trial));
            y.push(r.unit());
        }
        Array<u32> t;
        delaunay(x.data(), y.data(), n, &t);
        const usize tris = t.size() / 3;
        CHECK(tris >= n && tris <= 2 * n); // 2n - 2 - hull points, give or take the hull
        bool ccw = true, empty = true;
        Array<u8> seen;
        seen.resize(n);
        for (usize k = 0; k < tris; ++k) {
            const u32 a = t[3 * k], b = t[3 * k + 1], c = t[3 * k + 2];
            seen[a] = seen[b] = seen[c] = 1;
            ccw = ccw && orient(x.data(), y.data(), a, b, c) > 0;
            for (u32 p = 0; p < n && empty; ++p) {
                if (p == a || p == b || p == c)
                    continue;
                const double ax = x[a] - x[p], ay = y[a] - y[p], bx = x[b] - x[p], by = y[b] - y[p];
                const double cx = x[c] - x[p], cy = y[c] - y[p];
                const double det = (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
                                   (cx * cx + cy * cy) * (ax * by - bx * ay);
                empty = det <= 1e-12;
            }
        }
        CHECK(ccw);
        CHECK(empty);
        bool all = true;
        for (u32 i = 0; i < n; ++i)
            all = all && seen[i];
        CHECK(all);
    }
}

usize cell_of(const Landscape& L, double x, double y) {
    u32 ci = static_cast<u32>(x * L.raster_h), cj = static_cast<u32>(y * L.raster_h);
    ci = ci >= L.raster_w ? L.raster_w - 1 : ci;
    cj = cj >= L.raster_h ? L.raster_h - 1 : cj;
    return static_cast<usize>(cj) * L.raster_w + ci;
}

void test_land_and_nodes() {
    LandParams p;
    p.nodes = 1000;
    Landscape L;
    TextBuf err;
    CHECK(generate_landscape(5, p, &L, &err));
    const usize cells = static_cast<usize>(L.raster_w) * L.raster_h;
    usize land = 0;
    for (usize c = 0; c < cells; ++c)
        land += L.land[c];
    const double frac = static_cast<double>(land) / cells;
    CHECK(frac > p.fraction - 0.01 && frac < p.fraction + 0.01);
    CHECK(L.masses >= 1 && L.x.size() == 1000);
    bool on_land = true, spaced = true;
    for (u32 i = 0; i < 1000; ++i) {
        const usize c = cell_of(L, L.x[i], L.y[i]);
        on_land = on_land && L.land[c] && L.cell_mass[c] == L.mass[i];
        for (u32 j = i + 1; j < 1000 && spaced; ++j) {
            const double dx = L.x[i] - L.x[j], dy = L.y[i] - L.y[j];
            spaced = dx * dx + dy * dy >= L.spacing * L.spacing * 0.999999;
        }
    }
    CHECK(on_land);
    CHECK(spaced);

    // The same seed and settings give the same landscape; another seed doesn't.
    Landscape L2, L3;
    CHECK(generate_landscape(5, p, &L2, &err) && generate_landscape(6, p, &L3, &err));
    CHECK(L2.keys.size() == L.keys.size() && memcmp(L2.keys.data(), L.keys.data(), L.keys.size() * 8) == 0);
    CHECK(L3.keys.size() != L.keys.size() || memcmp(L3.keys.data(), L.keys.data(), L.keys.size() * 8) != 0);

    // Too many nodes for the land is refused, not looped on.
    LandParams tight = p;
    tight.nodes = 200000;
    tight.resolution = 32;
    Landscape L4;
    TextBuf e4;
    CHECK(!generate_landscape(5, tight, &L4, &e4) && e4.size() > 0);
}

// Paths: land paths stay on one landmass and never cross water; bridges join
// two landmasses; hermits have none; degrees spread from 0 to the hubs'.
void test_paths() {
    LandParams p;
    p.nodes = 1500;
    p.fraction = 0.35;
    p.scale = 4.0; // many islands
    p.paths_isolated = 0.03;
    p.paths_hubs = 0.03;
    Landscape L;
    TextBuf err;
    CHECK(generate_landscape(11, p, &L, &err));
    const u32 n = p.nodes;
    Array<u32> deg;
    deg.resize(n);
    bool land_ok = true, bridges_ok = true, sorted = true, symmetric = true;
    u32 bridge_edges = 0;
    for (usize e = 0; e < L.keys.size(); ++e) {
        const u32 a = static_cast<u32>(L.keys[e] >> 32), b = static_cast<u32>(L.keys[e]);
        ++deg[a];
        sorted = sorted && (e == 0 || L.keys[e] > L.keys[e - 1]);
        // the reverse direction exists with the same attributes
        usize lo = 0, hi = L.keys.size();
        const u64 rev = (static_cast<u64>(b) << 32) | a;
        while (lo < hi) {
            const usize mid = (lo + hi) / 2;
            if (L.keys[mid] < rev)
                lo = mid + 1;
            else
                hi = mid;
        }
        symmetric = symmetric && lo < L.keys.size() && L.keys[lo] == rev && L.latency[lo] == L.latency[e] &&
                    L.bridge[lo] == L.bridge[e] && L.phase[lo] == L.phase[e];
        if (L.bridge[e]) {
            ++bridge_edges;
            bridges_ok = bridges_ok && L.mass[a] != L.mass[b] && L.want[a] > 0 && L.want[b] > 0;
            continue;
        }
        if (L.mass[a] != L.mass[b]) {
            land_ok = false;
            continue;
        }
        // long links may cross a bay; the rest stay on land the whole way
        const double dx = L.x[b] - L.x[a], dy = L.y[b] - L.y[a];
        const u32 steps = static_cast<u32>(ceil(sqrt(dx * dx + dy * dy) * L.raster_h * 2.0)) + 1;
        u32 wet = 0;
        for (u32 s = 0; s <= steps; ++s) {
            const double t = static_cast<double>(s) / steps;
            wet += L.cell_mass[cell_of(L, L.x[a] + dx * t, L.y[a] + dy * t)] != L.mass[a];
        }
        land_ok = land_ok && (wet == 0 || L.long_added > 0);
    }
    CHECK(sorted && symmetric && land_ok && bridges_ok);
    CHECK(bridge_edges == 2 * L.bridges && L.bridges > 0);
    u32 zero = 0, one = 0, big = 0, hermits_ok = 1;
    for (u32 i = 0; i < n; ++i) {
        zero += deg[i] == 0;
        one += deg[i] == 1;
        big += deg[i] >= 8;
        if (L.want[i] == 0)
            hermits_ok &= deg[i] == 0;
    }
    CHECK(hermits_ok && L.hermits > 0 && zero >= L.hermits);
    CHECK(one > 0 && big > 0);
    printf("  landscape: %u nodes on %u landmasses, %u paths, %u bridges; %u with no path, %u with one, %u with 8+\n",
           n, L.masses, static_cast<unsigned>(L.keys.size() / 2), L.bridges, zero, one, big);

    // Each landmass in one piece when asked (hermits then get the tree's paths).
    LandParams c = p;
    c.paths_connected = true;
    c.paths_long = 0;
    Landscape C;
    CHECK(generate_landscape(11, c, &C, &err));
    Array<u32> comp;
    comp.resize(n);
    comp.fill(0xFFFFFFFFu);
    Array<u32> q;
    bool connected = true;
    for (u32 s = 0; s < n; ++s) {
        if (comp[s] != 0xFFFFFFFFu)
            continue;
        comp[s] = s;
        q.clear();
        q.push(s);
        for (usize qi = 0; qi < q.size(); ++qi) {
            const u32 u = q[qi];
            for (usize e = 0; e < C.keys.size(); ++e) {
                if (static_cast<u32>(C.keys[e] >> 32) != u || C.bridge[e])
                    continue;
                const u32 v = static_cast<u32>(C.keys[e]);
                if (comp[v] == 0xFFFFFFFFu) {
                    comp[v] = s;
                    q.push(v);
                }
            }
        }
    }
    // Every landmass is one component, and no component spans two.
    Array<u32> mass_comp;
    mass_comp.resize(C.masses);
    mass_comp.fill(0xFFFFFFFFu);
    for (u32 i = 0; i < n; ++i) {
        u32& mc = mass_comp[C.mass[i]];
        if (mc == 0xFFFFFFFFu)
            mc = comp[i];
        connected = connected && mc == comp[i];
    }
    for (u32 i = 0; i < n; ++i)
        connected = connected && C.mass[comp[i]] == C.mass[i];
    CHECK(connected);
}

void test_schedule_and_spread() {
    CHECK(bridge_open(0, 100, 10, 0) && bridge_open(9, 100, 10, 0) && !bridge_open(10, 100, 10, 0));
    CHECK(bridge_open(95, 100, 10, 5) && !bridge_open(95, 100, 10, 0) && bridge_open(123, 0, 0, 0));

    Array<double> x, y;
    Rng r(3, 0, 0);
    for (u32 i = 0; i < 500; ++i) {
        x.push(r.unit() * 1.6);
        y.push(r.unit());
    }
    Spread even;
    Array<double> w;
    spread_weights(9, kFieldSoupSize, even, x, y, 1.6, &w);
    bool ones = true;
    for (usize i = 0; i < w.size(); ++i)
        ones = ones && fabs(w[i] - 1.0) < 1e-12;
    CHECK(ones);
    Spread s;
    s.spread = 1.0;
    s.smooth = 0.6;
    s.barren = 0.1;
    s.hotspots = 3;
    s.boost = 4;
    spread_weights(9, kFieldSoupSize, s, x, y, 1.6, &w);
    u32 barren = 0;
    double sum = 0, top = 0;
    for (usize i = 0; i < w.size(); ++i) {
        barren += w[i] == 0;
        sum += w[i];
        top = w[i] > top ? w[i] : top;
    }
    CHECK(barren == 50 && fabs(sum - 500.0) < 1e-6 && top > 4);
    Array<u64> shares;
    spread_total(w, 1000003, 64, &shares);
    u64 total = 0, at_min = 0;
    bool min_ok = true;
    for (usize i = 0; i < shares.size(); ++i) {
        total += shares[i];
        min_ok = min_ok && shares[i] >= 64;
        at_min += shares[i] == 64;
    }
    CHECK(total == 1000003 && min_ok && at_min >= 50);
    // Deterministic.
    Array<double> w2;
    spread_weights(9, kFieldSoupSize, s, x, y, 1.6, &w2);
    CHECK(memcmp(w.data(), w2.data(), w.size() * sizeof(double)) == 0);
}

// A small, busy landscape world: dispersal, quickly cycling bridges, signals,
// link failures, the lifecycle, and every resource spread unevenly with barren
// nodes (GPU and NPU included), and a seeded replicator so cargo flows.
const char* const kLandWorld = "seed = 7\n"
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
                               "bridge.phase = random\n"
                               "world.soup_size = 2048\n"
                               "world.millicores = 600\n"
                               "world.gpus = 60\n"
                               "world.npus = 40\n"
                               "res.memory.spread = 0.6\n"
                               "res.memory.smooth = 0.5\n"
                               "res.cpu.spread = 0.8\n"
                               "res.cpu.barren = 0.05\n"
                               "res.gpu.spread = 1\n"
                               "res.gpu.barren = 0.3\n"
                               "res.gpu.hotspots = 2\n"
                               "res.npu.barren = 0.5\n"
                               "spark.spread = 0.7\n"
                               "spark.barren = 0.2\n"
                               "edge.bandwidth = 8\n"
                               "edge.failure = 0.003\n"
                               "edge.downtime = 20\n"
                               "signal.channels = 4\n"
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

// The same world at 1, 4 and 20 threads and under shuffled dispatch; a
// snapshot taken midway resumes exactly.
void test_world_determinism() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, kLandWorld, threads[v]));
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
        run.run_until(300);
        hashes[v] = run.world().hash();
        if (v == 0) {
            u32 bodies = 0;
            for (u32 n = 0; n < run.world().node_count(); ++n)
                bodies += static_cast<u32>(run.world().node_soup(n).body_count());
            printf("  landscape world: 300 ticks, %u bodies, %llu collapses, hash %016llx\n", bodies,
                   static_cast<unsigned long long>(run.world().collapses_total()),
                   static_cast<unsigned long long>(hashes[0]));
            CHECK(bodies > 0);
        }
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);

    Run a;
    CHECK(make_run(a, kLandWorld, 4));
    a.run_until(150);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(300);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
}

// Bridges shut on schedule, nothing leaves on a shut bridge, nothing travels
// to or from a fallow site, and the paths never change.
void test_world_bridges_and_lifecycle() {
    Run run;
    CHECK(make_run(run, kLandWorld, 4));
    const World& w = run.world();
    const u32 edges = w.edge_count();
    Array<u64> keys;
    for (u32 e = 0; e < edges; ++e)
        keys.push((static_cast<u64>(w.edge_src(e)) << 32) | w.edge_dst(e));
    const LandParams& p = w.land_params();
    bool schedule = true, departures = true, fallow_clear = true, ports = true;
    u64 bridge_cargo = 0;
    u32 bridges = 0;
    for (u32 e = 0; e < edges; ++e)
        bridges += w.edge_bridge(e);
    CHECK(bridges > 0);
    // Bridges are numbered after a node's land paths.
    for (u32 n = 0; n < w.node_count(); ++n) {
        bool seen_bridge = false;
        for (u32 k = 0; k < w.degree(n); ++k) {
            const bool b = w.edge_bridge(w.port(n, k));
            ports = ports && (!seen_bridge || b);
            seen_bridge = seen_bridge || b;
        }
    }
    CHECK(ports);
    for (u32 t = 0; t < 300; ++t) {
        for (u32 e = 0; e < edges; ++e)
            if (w.edge_bridge(e))
                schedule = schedule && w.edge_shut(e) == !bridge_open(w.tick(), p.open_every, p.open_ticks, w.edge_phase(e));
        run.step();
        for (usize i = 0; i < w.in_flight(); ++i) {
            const u32 e = w.cargo_edge(i);
            if (!w.live(w.edge_src(e)) || !w.live(w.edge_dst(e)))
                fallow_clear = false;
            if (!w.edge_bridge(e))
                continue;
            ++bridge_cargo;
            const u64 sent = w.cargo_due(i) - w.cargo_ticks(i); // left during this tick
            departures = departures && bridge_open(sent, p.open_every, p.open_ticks, w.edge_phase(e));
        }
    }
    CHECK(schedule && departures && fallow_clear);
    CHECK(w.collapses_total() > 0);
    bool same = w.edge_count() == edges;
    for (u32 e = 0; same && e < edges; ++e)
        same = keys[e] == ((static_cast<u64>(w.edge_src(e)) << 32) | w.edge_dst(e));
    CHECK(same);
    printf("  landscape world: %u bridge edges, %llu cargo-ticks on bridges, %llu collapses, %llu recoveries\n",
           bridges, static_cast<unsigned long long>(bridge_cargo),
           static_cast<unsigned long long>(w.collapses_total()),
           static_cast<unsigned long long>(w.recoveries_total()));
    // Every soup still keeps its books, barren GPU and NPU nodes included.
    bool books = true;
    for (u32 n = 0; n < w.node_count() && books; ++n) {
        TextBuf why;
        books = w.node_soup(n).check(&why);
        if (!books)
            fprintf(stderr, "  node %u: %s\n", n, why.c_str());
    }
    CHECK(books);
}

// Each resource adds up to its total exactly; barren nodes get none (memory:
// the minimum soup); the resource keys of vary.* are not read here.
void test_world_distributions() {
    Run run;
    CHECK(make_run(run, kLandWorld, 1));
    const World& w = run.world();
    u64 mem = 0, cpu = 0, gpu = 0, npu = 0;
    u32 no_gpu = 0, no_npu = 0, no_sparks = 0, min_mem = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const SoupParams& p = w.node_params(n);
        mem += p.size;
        cpu += p.millicores;
        gpu += p.gpus;
        npu += p.npus;
        no_gpu += p.gpus == 0;
        no_npu += p.npus == 0;
        no_sparks += p.spark_rate == 0;
        min_mem += p.size == 64;
    }
    CHECK(mem == 2048ull * 90 && cpu == 600ull * 90 && gpu == 60ull * 90 && npu == 40ull * 90);
    CHECK(no_gpu >= 27 && no_npu >= 45 && no_sparks == 18);
    TextBuf cfg;
    cfg.append(kLandWorld);
    cfg.append("vary.soup_size = 0.5\n");
    Run bad;
    Config::ParseError pe;
    CHECK(bad.config().parse(cfg.view(), &pe));
    TextBuf err;
    CHECK(bad.init(1, &err));
    Str unused;
    CHECK(bad.config().first_unused(&unused) && unused.eq("vary.soup_size"));
}

// stats.nodes_every: node rows every Nth window, each covering every tick
// since the last; the end writes what's owed.
void test_nodes_every() {
    const char* stats = "nova_test_land_stats.csv";
    const char* nodes = "nova_test_land_nodes.csv";
    Run run;
    CHECK(make_run(run, kSoupWorld1b, 2));
    TextBuf err;
    CHECK(run.open_stats(stats, 10, &err));
    CHECK(run.open_nodes(nodes, &err, 3));
    run.run_until(70);
    run.finish();
    Array<u8> bytes;
    CHECK(read_file(nodes, &bytes));
    // tick_first,tick_last of each row (after the header)
    u64 firsts[8] = {}, lasts[8] = {};
    u32 sets = 0, rows = 0;
    usize at = 0;
    while (at < bytes.size() && bytes[at] != '\n')
        ++at;
    ++at;
    while (at < bytes.size()) {
        u64 a = 0, b = 0;
        while (bytes[at] != ',')
            a = a * 10 + (bytes[at++] - '0');
        ++at;
        while (bytes[at] != ',')
            b = b * 10 + (bytes[at++] - '0');
        while (at < bytes.size() && bytes[at] != '\n')
            ++at;
        ++at;
        if (rows % 24 == 0 && sets < 8) {
            firsts[sets] = a;
            lasts[sets] = b;
            ++sets;
        }
        ++rows;
    }
    CHECK(rows == 3 * 24 && sets == 3);
    CHECK(firsts[0] == 0 && lasts[0] == 29 && firsts[1] == 30 && lasts[1] == 59 && firsts[2] == 60 &&
          lasts[2] == 69);
    remove(stats);
    remove(nodes);
}

} // namespace

void suite_landscape() {
    test_delaunay();
    test_land_and_nodes();
    test_paths();
    test_schedule_and_spread();
    test_world_determinism();
    test_world_bridges_and_lifecycle();
    test_world_distributions();
    test_nodes_every();
}
