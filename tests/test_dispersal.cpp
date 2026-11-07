// The world (Phase 3, design.md 6.9): procedural graph generation (determinism,
// k-nearest links, ruggedness cuts, long links, edge latency and bandwidth,
// neighbour numbering, node properties from noise); DIVIDE to a neighbour (the
// exchange rule at both ends, edge capacity, numbering, SENSE 14); transit
// timing; lineages across nodes and the colonist event; snapshot resume with
// cargo in flight; and event replay. The determinism gate on a procedural
// world with dispersal is in test_gate.cpp.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/graph.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// A procedural soup world with nothing alive: the graph and properties only.
const char* const kGraphWorld = "seed = 7\n"
                                "world.model = soup\n"
                                "world.graph = procedural\n"
                                "world.nodes = 100\n"
                                "world.soup_size = 1024\n"
                                "world.millicores = 1000\n"
                                "world.gpus = 40\n"
                                "detect.enabled = false\n";

const char* const kVaryAll = "vary.soup_size = 0.5\n"
                             "vary.millicores = 0.5\n"
                             "vary.gpus = 0.5\n"
                             "vary.cool = 0.5\n"
                             "vary.spark_rate = 0.5\n"
                             "vary.radiation = 0.5\n"
                             "spark.rate = 0.2\n";

bool has_edge(const World& w, u32 a, u32 b) {
    for (u32 e = w.out_begin(a); e < w.out_end(a); ++e)
        if (w.edge_dst(e) == b)
            return true;
    return false;
}

double dist(const World& w, u32 a, u32 b) {
    const double dx = w.node_x(a) - w.node_x(b), dy = w.node_y(a) - w.node_y(b);
    return sqrt(dx * dx + dy * dy);
}

// Generation is a function of the seed; links are symmetric; with no
// ruggedness and no long links every node links to its k nearest; ruggedness
// cuts about its fraction; edges carry latency and bandwidth by the formulas;
// neighbours are numbered nearest first.
void graph() {
    {
        TextBuf cfg;
        cfg.append(kGraphWorld);
        cfg.append("world.ruggedness = 0\nworld.long_links = 0\n");
        Run a, b;
        CHECK(make_run(a, cfg.c_str(), 1));
        CHECK(make_run(b, cfg.c_str(), 4));
        const World& w = a.world();
        CHECK(w.procedural() && w.dispersal() && w.edge_count() == b.world().edge_count());
        CHECK(w.links_cut() == 0 && w.long_links() == 0);
        CHECK_EQ_U64(w.hash(), b.world().hash());
        bool knn = true, symmetric = true, ranged = true;
        for (u32 n = 0; n < w.node_count(); ++n) {
            ranged = ranged && w.node_x(n) > 0 && w.node_x(n) < 1 && w.node_y(n) > 0 && w.node_y(n) < 1;
            // Its 4 nearest, by brute force (ties: the lower id).
            u32 best[4] = {};
            double bd[4] = {};
            u32 found = 0;
            for (u32 m = 0; m < w.node_count(); ++m) {
                if (m == n)
                    continue;
                const double d = dist(w, n, m);
                if (found == 4 && d >= bd[3])
                    continue;
                u32 at = found < 4 ? found : 3;
                while (at > 0 && bd[at - 1] > d) {
                    best[at] = best[at - 1];
                    bd[at] = bd[at - 1];
                    --at;
                }
                best[at] = m;
                bd[at] = d;
                if (found < 4)
                    ++found;
            }
            for (u32 k = 0; k < 4; ++k)
                knn = knn && has_edge(w, n, best[k]);
            for (u32 e = w.out_begin(n); e < w.out_end(n); ++e)
                symmetric = symmetric && has_edge(w, w.edge_dst(e), n);
        }
        CHECK(knn && symmetric && ranged);
    }
    {
        TextBuf cfg;
        cfg.append(kGraphWorld);
        cfg.append("world.ruggedness = 0.3\nedge.latency_scale = 20\nedge.bandwidth = 32\n");
        Run a;
        CHECK(make_run(a, cfg.c_str(), 1));
        const World& w = a.world();
        // About 30% of the proximity links are cut; 2 long links (0.02 x 100).
        const u32 links = w.edge_count() / 2;
        const u32 proximity = links - w.long_links() + w.links_cut();
        CHECK(w.long_links() == 2);
        CHECK(w.links_cut() * 10 >= proximity * 2 && w.links_cut() * 10 <= proximity * 4);
        bool formulas = true, nearest_first = true, symmetric = true;
        u32 isolated = 0, max_latency = 0;
        for (u32 e = 0; e < w.edge_count(); ++e) {
            const double d = dist(w, w.edge_src(e), w.edge_dst(e));
            formulas = formulas && w.edge_latency(e) == 1 + static_cast<u32>(floor(d * 20)) &&
                       w.edge_bandwidth(e) >= 8 && w.edge_bandwidth(e) <= 32;
            max_latency = max_of(max_latency, w.edge_latency(e));
        }
        for (u32 n = 0; n < w.node_count(); ++n) {
            isolated += w.degree(n) == 0;
            for (u32 k = 1; k < w.degree(n); ++k)
                nearest_first = nearest_first && w.edge_distance(w.port(n, k - 1)) <= w.edge_distance(w.port(n, k));
            for (u32 k = 0; k < w.degree(n); ++k) {
                const u32 e = w.port(n, k);
                symmetric = symmetric && w.edge_src(e) == n && has_edge(w, w.edge_dst(e), n);
            }
        }
        CHECK(formulas && nearest_first && symmetric);
        CHECK(max_latency >= 5); // the long links are slow
        printf("  dispersal: 100-node graph: %u links (%u cut by ruggedness, %u long), %u isolated, "
               "max latency %u\n",
               links, w.links_cut(), w.long_links(), isolated, max_latency);

        // Another seed, another graph.
        TextBuf other;
        other.append("seed = 8\n");
        other.append(cfg.c_str() + strlen("seed = 7\n"));
        Run b;
        CHECK(make_run(b, other.c_str(), 1));
        CHECK(b.world().node_x(0) != w.node_x(0));
    }
    {
        // Noise: deterministic per (seed, field), coherent, and the node field
        // spans -1..1.
        Noise2 n1, n2, n3;
        n1.init(7, 1);
        n2.init(7, 1);
        n3.init(7, 2);
        CHECK(n1.fbm(0.3, 0.4) == n2.fbm(0.3, 0.4) && n1.fbm(0.3, 0.4) != n3.fbm(0.3, 0.4));
        CHECK(fabs(n1.fbm(0.3, 0.4) - n1.fbm(0.3001, 0.4)) < 0.01);
        Array<double> x, y, z;
        for (u32 i = 0; i < 10; ++i) {
            x.push(0.05 + 0.1 * i);
            y.push(0.5);
        }
        node_field(7, 1, x, y, &z);
        double lo = 2, hi = -2;
        for (usize i = 0; i < z.size(); ++i) {
            lo = z[i] < lo ? z[i] : lo;
            hi = z[i] > hi ? z[i] : hi;
        }
        CHECK(lo == -1.0 && hi == 1.0);
    }
}

// vary.* spreads each property over its field: the extremes are exactly the
// mean x (1 -/+ f); with no vary every node has the configured values.
void properties() {
    {
        TextBuf cfg;
        cfg.append(kGraphWorld);
        cfg.append(kVaryAll);
        Run a;
        CHECK(make_run(a, cfg.c_str(), 1));
        const World& w = a.world();
        u32 lo_size = ~0u, hi_size = 0, lo_mc = ~0u, hi_mc = 0, lo_gpu = ~0u, hi_gpu = 0;
        double spark_sum = 0, lo_cool = 2, hi_cool = 0, lo_rad = 2, hi_rad = 0;
        bool sizes_ok = true;
        for (u32 n = 0; n < w.node_count(); ++n) {
            const SoupParams& p = w.node_params(n);
            lo_size = min_of(lo_size, p.size);
            hi_size = max_of(hi_size, p.size);
            lo_mc = min_of(lo_mc, p.millicores);
            hi_mc = max_of(hi_mc, p.millicores);
            lo_gpu = min_of(lo_gpu, p.gpus);
            hi_gpu = max_of(hi_gpu, p.gpus);
            lo_cool = p.cool < lo_cool ? p.cool : lo_cool;
            hi_cool = p.cool > hi_cool ? p.cool : hi_cool;
            const double rad = p.copy_rate / w.soup_params().copy_rate;
            lo_rad = rad < lo_rad ? rad : lo_rad;
            hi_rad = rad > hi_rad ? rad : hi_rad;
            spark_sum += p.spark_rate;
            sizes_ok = sizes_ok && w.node_soup(n).size() == p.size && p.search_limit <= p.size;
        }
        CHECK(lo_size == 512 && hi_size == 1536 && lo_mc == 500 && hi_mc == 1500);
        CHECK(lo_gpu == 20 && hi_gpu == 60 && sizes_ok);
        CHECK(fabs(lo_cool - 0.05) < 1e-6 && fabs(hi_cool - 0.15) < 1e-6);
        CHECK(fabs(lo_rad - 0.5) < 1e-9 && fabs(hi_rad - 1.5) < 1e-9);
        CHECK(fabs(spark_sum / w.node_count() - 0.2) < 1e-9); // evenly spread: the mean stays
        // A live spark.rate change reaches every node, times its factor.
        TextBuf err;
        double before = w.node_params(5).spark_rate;
        CHECK(a.schedule_knob(0, "spark.rate", "0.4", &err));
        a.step();
        CHECK(fabs(w.node_params(5).spark_rate - 2 * before) < 1e-12);
    }
    {
        TextBuf cfg;
        cfg.append(kGraphWorld);
        Run a;
        CHECK(make_run(a, cfg.c_str(), 1));
        bool uniform = true;
        const SoupParams& m = a.world().soup_params();
        for (u32 n = 0; n < a.world().node_count(); ++n) {
            const SoupParams& p = a.world().node_params(n);
            uniform = uniform && p.size == m.size && p.millicores == m.millicores && p.gpus == m.gpus &&
                      p.cool == m.cool && p.spark_rate == m.spark_rate && p.copy_rate == m.copy_rate &&
                      p.rot_rate == m.rot_rate && p.search_limit == m.search_limit;
        }
        CHECK(uniform);
    }
    {
        // world.origins = one: only node 0 gets sparks; seed.node: one founder.
        TextBuf cfg;
        cfg.append(kGraphWorld);
        cfg.append("world.origins = one\nspark.rate = 0.5\nspark.initial = 10\nspark.max_len = 32\n"
                   "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE\nseed.node = 3\n");
        Run a;
        CHECK(make_run(a, cfg.c_str(), 1));
        const World& w = a.world();
        CHECK(w.node_params(0).spark_rate == 0.5 && w.node_params(1).spark_rate == 0);
        CHECK(w.node_soup(0).body_count() == 10 && w.node_soup(1).body_count() == 0);
        CHECK(w.node_soup(3).body_count() == 1 && w.node_params(4).seed_len == 0);
        a.run_until(20);
        CHECK(w.node_soup(1).tally().sparks == 0);
    }
}

// A quiet soup with dispersal on, `degree` ports (edge ids 10, 11, ...) of
// capacity `cap`, and a program at 100 with 500 millicores.
struct Fix {
    SoupParams p;
    Soup s;
    u32 edges[8] = {10, 11, 12, 13, 14, 15, 16, 17};
    u32 in_flight[32] = {};
    u32 b = kNone, c = kNone;

    bool init(const char* prog, u32 degree, u32 cap, bool dispersal = true) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.gpus = 100\n");
        cfg.append(dispersal ? "world.dispersal = true\n" : "");
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 5, 0);
        DispersalPorts P;
        P.edges = edges;
        P.in_flight = in_flight;
        P.degree = degree;
        P.capacity = cap;
        s.set_ports(P);
        b = plant(s, 100, prog, 500);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        s.claim_gpus(b, 40);
        s.prepare_gpu();
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    void op(u32 ip, u32 bx, u32 cx) {
        cpu().ip = ip;
        cpu().bx = bx;
        cpu().cx = cx;
        s.run_cpu(c, 1);
    }
    // ALLOC 8 codons, then DIVIDE with (bx, share).
    bool alloc_divide(u32 bx, u32 share) {
        op(100, 0, 8);
        if (cpu().err)
            return false;
        op(101, bx, share);
        return cpu().err == 0;
    }
};

// The exchange rule at the origin: the daughter's codons become cargo, her
// block becomes waste (contents kept), her share of millicores and GPU units
// goes back into the pools; capacity, numbering and the degree are enforced.
void exchange() {
    Fix f;
    CHECK(f.init("ALLOC DIVIDE", 2, 1));
    const u32 waste0 = f.s.waste(), pool0 = f.s.free_units(), gpool0 = f.s.free_gpus();
    f.op(100, 0, 8); // ALLOC 8
    const u32 k = f.s.body(f.b).latest;
    const u32 start = f.s.block(k).start;
    u8 before[8];
    for (u32 i = 0; i < 8; ++i)
        before[i] = f.s.mem()[start + i];
    f.op(101, 1, 16); // DIVIDE to neighbour 1, share 16/64
    CHECK(f.cpu().err == 0);
    CHECK(f.s.departures().size() == 1 && f.s.tally().sent == 1 && f.s.tally().births == 0);
    const Departure& d = f.s.departures()[0];
    CHECK(d.nbr == 0 && d.len == 8 && d.millicores == 125 && d.gpus == 10 && d.npus == 0);
    CHECK(memcmp(f.s.departure_codons() + d.codon_off, before, 8) == 0);
    CHECK(memcmp(f.s.mem() + start, before, 8) == 0); // waste keeps its contents
    CHECK(f.s.owner_block(start) == kNone && f.s.waste() == waste0);
    CHECK(f.s.body(f.b).units == 375 && f.s.free_units() == pool0 + 125);
    CHECK(f.s.gpu_count(f.b) == 30 && f.s.free_gpus() == gpool0 + 10);
    CHECK(f.s.body_count() == 1 && f.s.body(f.b).latest == kNone && f.s.body(f.b).mem == 2);
    CHECK(soup_ok(f.s));

    CHECK(!f.alloc_divide(1, 16)); // edge 1 full: this tick's departure fills it
    CHECK(f.s.departures().size() == 1 && f.s.body(f.b).latest != kNone); // nothing changed
    f.op(101, 2, 0); // the block from the failed try goes to neighbour 2
    CHECK(f.s.departures().size() == 2 && f.s.departures()[1].nbr == 1 && f.s.departures()[1].millicores == 0);
    f.op(100, 0, 8);
    f.op(101, 3, 0); // BX 3 -> neighbour 3: past the degree
    CHECK(f.cpu().err == 1);
    f.op(101, 7, 0); // BX >= 4 is neighbour BX directly: past the degree too
    CHECK(f.cpu().err == 1 && f.s.departures().size() == 2);
    CHECK(soup_ok(f.s));

    // Edge capacity counts what is already in flight.
    Fix g;
    CHECK(g.init("ALLOC DIVIDE", 6, 2));
    g.in_flight[10] = 2;
    CHECK(!g.alloc_divide(1, 0));
    g.in_flight[10] = 1;
    g.op(101, 1, 0);
    CHECK(g.cpu().err == 0 && g.s.departures().size() == 1);
    CHECK(g.alloc_divide(5, 0)); // direct numbering: neighbour 5
    CHECK(g.s.departures().size() == 2 && g.s.departures()[1].nbr == 4);
    CHECK(g.alloc_divide(0, 8)); // BX 0 = here: an ordinary birth
    CHECK(g.s.tally().births == 1 && g.s.body_count() == 2);

    // Without dispersal anything but here fails, as before.
    Fix h;
    CHECK(h.init("ALLOC DIVIDE", 2, 2, false));
    CHECK(!h.alloc_divide(1, 0) && h.s.departures().size() == 0);
}

// SENSE 14 is my node's degree, with dispersal; without it SENSE has 14
// readings at most, and reading 14 fails.
void sense14() {
    Fix f;
    CHECK(f.init("SENSE", 3, 2));
    CHECK(f.p.sel.sense_n == 15);
    f.op(100, 14, 0);
    CHECK(f.cpu().err == 0 && f.cpu().cx == 3);
    Fix g;
    CHECK(g.init("SENSE", 3, 2, false));
    CHECK(g.p.sel.sense_n == 11); // a GPU, no NPU
    g.op(100, 14, 0);
    CHECK(g.cpu().err == 1);
}

// The exchange rule at the destination: a random waste run of her length, her
// shares drawn from the pools (capped), one fresh CPU at her start; no room
// and she is lost.
void arrival() {
    Fix f;
    CHECK(f.init("ALLOC DIVIDE", 2, 2));
    const u8 code[6] = {1, 2, 3, 4, 5, 6};
    CargoTag tag = {};
    const u32 bodies = static_cast<u32>(f.s.body_count());
    const u32 pool = f.s.free_units();
    CHECK(f.s.arrive(code, 6, 100, 7, 3, 0, tag));
    CHECK(f.s.body_count() == bodies + 1 && f.s.tally().arrived == 1);
    const u32 nb = f.s.body_at(f.s.body_count() - 1);
    const Block& B = f.s.block(f.s.body(nb).blocks);
    CHECK(B.len == 6 && memcmp(f.s.mem() + B.start, code, 6) == 0);
    CHECK(f.s.body(nb).units == 100 && f.s.free_units() == pool - 100);
    CHECK(f.s.gpu_count(nb) == 7); // no NPU on this node: none drawn
    const Cpu& C = f.s.cpu(f.s.body(nb).cpus);
    CHECK(C.ip == B.start && C.ax == 0 && C.bx == 0 && C.cx == 0 && C.sp == 0 && f.s.body(nb).ncpu == 1);
    CHECK(f.s.arrive(code, 6, 100000, 0, 0, 0, tag)); // capped by the pool
    CHECK(f.s.free_units() == 0);
    CHECK(soup_ok(f.s));

    // No room: a soup full of one body.
    SoupParams p;
    CHECK(make_params(&p, "world.millicores = 100\nmem.upkeep = 0\n", 1024));
    Soup s;
    s.init(&p, 1, 0);
    s.begin_tick(0);
    CHECK(s.create_body(0, 1024, 10) != kNone);
    CHECK(!s.arrive(code, 6, 10, 0, 0, 0, tag) && s.tally().lost == 1 && s.body_count() == 1);
}

// A one-shot disperser: ALLOC 8, then DIVIDE to neighbour 1 with share 32/64,
// then eight failing DIVIDEs halt it.
const char* const kOneShot = "seed.genome = ZERO INC SHL SHL SHL ALLOC ZERO INC ROTB ZERO INC SHL SHL SHL "
                             "SHL SHL DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE\n";

// Transit: a cargo of L codons sent during tick t is placed at the barrier
// ending tick t + ceil(L / bandwidth) + latency.
void transit() {
    TextBuf cfg;
    cfg.append("seed = 3\nworld.model = soup\nworld.nodes = 2\nworld.soup_size = 512\n"
               "world.millicores = 1000\nworld.dispersal = true\nedge.bandwidth = 4\n"
               "seed.node = 0\nseed.at = 100\nseed.millicores = 1000\n");
    cfg.append(kQuiet);
    cfg.append(kOneShot);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 2));
    const World& w = run.world();
    CHECK(w.dispersal() && !w.procedural() && w.degree(0) == 1 && w.edge_latency(0) == 1);
    run.step(); // tick 0: she leaves
    CHECK(w.last_soup_tick().sent == 1 && w.in_flight() == 1 && w.node_soup(0).body_count() == 0);
    CHECK(w.node_soup(0).free_units() == 1000); // the parent died; the pools have it all
    run.step(); // tick 1
    run.step(); // tick 2
    CHECK(w.in_flight() == 1 && w.node_soup(1).body_count() == 0);
    run.step(); // tick 3 = 0 + ceil(8 / 4) + 1: placed at its barrier
    CHECK(w.last_soup_tick().arrived == 1 && w.in_flight() == 0);
    CHECK(w.node_soup(1).body_count() == 1);
    const Soup& d = w.node_soup(1);
    CHECK(d.body(d.body_at(0)).units == 500 && d.block(d.body(d.body_at(0)).blocks).len == 8);
}

// A dispersing replicator: copies itself, then DIVIDEs to its nearest
// neighbour with a share of its memory / 64.
const char* const kDisperser = "ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE";

const char* const kColonyWorld = "seed = 11\n"
                                 "world.model = soup\n"
                                 "world.graph = procedural\n"
                                 "world.nodes = 12\n"
                                 "world.soup_size = 2048\n"
                                 "world.millicores = 2000\n"
                                 "cpu.instr_per_core = 2000\n"
                                 "mem.upkeep = 0\n"
                                 "heat.enabled = false\n"
                                 "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                                 "world.ruggedness = 0\n"
                                 "edge.bandwidth = 8\n"
                                 "seed.node = 0\n"
                                 "seed.at = 1000\n"
                                 "seed.millicores = 1000\n";

// Lineages span nodes: a dispersed daughter keeps her lineage and depth, the
// lineage id carries its founding node, and the first gated arrival on a node
// the lineage wasn't founded on is a colonist event, once per node.
void colonists() {
    TextBuf cfg;
    cfg.append(kColonyWorld);
    cfg.append("seed.genome = ");
    cfg.append(kDisperser);
    cfg.append_char('\n');
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    run.run_until(60);
    const World& w = run.world();
    u64 colonisations = 0, foreign = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const Lineages* L = w.node_lineages(n);
        colonisations += L->colonisations();
        LineageSurvey sv;
        L->survey(&sv);
        foreign += sv.colonists;
        CHECK(n == 0 || L->founded() == 0); // only the seed's node founds: the rest are colonised
    }
    const Detection& c = run.colonist();
    CHECK(c.found && lineage_node(c.lineage) == 0 && c.node != 0);
    CHECK(colonisations > 0 && foreign > 0);
    usize col_events = 0;
    for (usize i = 0; i < run.event_count(); ++i)
        col_events += run.event_text(i).starts_with("colonist node=");
    CHECK(col_events == colonisations);
    printf("  dispersal: seeded disperser on 1 of 12 nodes: first colonist at tick %llu on node %u "
           "(lineage %u.%u), %llu colonisations by tick 60\n",
           static_cast<unsigned long long>(c.tick), c.node, lineage_node(c.lineage),
           static_cast<u32>(c.lineage), static_cast<unsigned long long>(colonisations));

    // Detection on or off, the world is the same.
    TextBuf off;
    off.append(cfg.c_str());
    off.append("detect.enabled = false\n");
    Run quiet;
    CHECK(make_run(quiet, off.c_str(), 1));
    quiet.run_until(60);
    CHECK_EQ_U64(quiet.world().hash(), w.hash());
}

// Snapshot resume with cargo in flight equals running straight through.
void snapshot_cargo() {
    TextBuf cfg;
    cfg.append(kColonyWorld);
    cfg.append("seed.genome = ");
    cfg.append(kDisperser);
    cfg.append_char('\n');
    Run straight;
    CHECK(make_run(straight, cfg.c_str(), 1));
    straight.run_until(80);

    Run first;
    CHECK(make_run(first, cfg.c_str(), 4));
    u64 at = 0;
    for (u64 t = 0; t < 60; ++t) { // stop at a tick with cargo in flight
        first.step();
        if (first.world().in_flight() > 0 && first.world().tick() >= 20) {
            at = first.world().tick();
            break;
        }
    }
    CHECK(at > 0 && first.world().in_flight() > 0);
    ByteWriter bytes;
    first.snapshot(bytes);
    Run resumed;
    TextBuf err;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
    CHECK(resumed.world().in_flight() == first.world().in_flight());
    CHECK_EQ_U64(resumed.world().hash(), first.world().hash());
    resumed.run_until(80);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
}

// Colonist events go into the replay log and replay exactly.
void replay_events() {
    const char* path = "nova_test_dispersal.replay";
    TextBuf cfg;
    cfg.append(kColonyWorld);
    cfg.append("seed.genome = ");
    cfg.append(kDisperser);
    cfg.append_char('\n');
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    TextBuf err;
    CHECK(run.open_replay(path, 10, &err));
    run.run_until(50);
    run.finish();
    ReplayLog log;
    CHECK(log.load_file(path, &err));
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r));
    CHECK(r.ok && r.events_checked == run.event_count() && run.event_count() > 0);
    CHECK(replay_verify(log, 20, &r));
    remove(path);
}

// The CSV columns and the per-node file.
void outputs() {
    const char* csv = "nova_test_world.csv";
    const char* nodes = "nova_test_nodes.csv";
    TextBuf cfg;
    cfg.append(kColonyWorld);
    cfg.append("seed.genome = ");
    cfg.append(kDisperser);
    cfg.append_char('\n');
    Run run;
    CHECK(make_run(run, cfg.c_str(), 1));
    TextBuf err;
    CHECK(run.open_stats(csv, 10, &err));
    CHECK(run.open_nodes(nodes, &err));
    run.run_until(30);
    run.finish();
    Array<u8> bytes;
    CHECK(read_file(csv, &bytes));
    bytes.push(0);
    const char* text = reinterpret_cast<const char*>(bytes.data());
    CHECK(strstr(text, ",npu_worn,sent,arrived,lost,") != nullptr);
    CHECK(strstr(text, ",npu_held,in_flight,links,") != nullptr);
    CHECK(strstr(text, ",genotypes_in_top,colonised,links_down,disk_held,live_nodes,warp_steps,lane_instr,"
                       "warp_host,launches,gpu_births,warps,gpu_bodies,actions_started,actions_landed,"
                       "actions_failed,actions_defended,threads_started,e_work,e_held,vent_codons,decayed,bites_waste,bites_body,codons_bitten,threads_ended,died_halted,died_frozen,died_worn,dormant_started,dormant_woke,dormant_expired,dormant_eaten,dormant_hops,drifting,resting,compress,ambient\n") !=
          nullptr);
    CHECK(read_file(nodes, &bytes));
    bytes.push(0);
    text = reinterpret_cast<const char*>(bytes.data());
    CHECK(strncmp(text, "tick_first,tick_last,node,x,y,degree,", 37) == 0);
    usize rows = 0;
    for (const char* p = text; *p; ++p)
        rows += *p == '\n';
    CHECK(rows == 1 + 3 * 12); // header, then 12 nodes x 3 windows
    remove(csv);
    remove(nodes);
}

} // namespace

void suite_dispersal() {
    graph();
    properties();
    exchange();
    sense14();
    arrival();
    transit();
    colonists();
    snapshot_cargo();
    replay_events();
    outputs();
}
