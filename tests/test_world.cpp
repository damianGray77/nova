#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/world.h"

using namespace nova;

namespace {

bool init_world(World& w, const char* text, Config& c) {
    Config::ParseError e;
    if (!c.parse(text, &e))
        return false;
    return w.init(c);
}

void test_defaults_and_validation() {
    World w;
    Config c;
    CHECK(init_world(w, "", c)); // every key has a default
    CHECK(w.node_count() == 64 && w.soup_size() == 256 && w.tick() == 0);
    CHECK(w.total_units() == 64ull * 500);

    const char* bad[] = {
        "world.nodes = 0\n",           "world.pool_capacity = 100\nworld.pool_initial = 101\n",
        "world.outbox_capacity = 0\n", "demo.send_rate = 1.5\n",
        "demo.max_parcel = 0\n",       "seed = forty-two\n",
    };
    for (const char* text : bad) {
        World b;
        Config bc;
        CHECK(!init_world(b, text, bc));
        CHECK(!bc.ok());
    }
    World b;
    Config bc;
    init_world(b, "world.nodes = 0\n", bc);
    CHECK(strstr(bc.value_error(), "world.nodes") != nullptr);
}

void test_graph() {
    World w;
    Config c;
    CHECK(init_world(w, kBusyWorld, c));
    bool ok = true;
    for (u32 e = 0; e < w.edge_count(); ++e) {
        u32 s = w.edge_src(e), d = w.edge_dst(e);
        ok = ok && s != d && s < w.node_count() && d < w.node_count();
        if (e > 0) {
            u64 prev = (static_cast<u64>(w.edge_src(e - 1)) << 32) | w.edge_dst(e - 1);
            ok = ok && ((static_cast<u64>(s) << 32) | d) > prev; // sorted, no duplicates
        }
        // every link exists in both directions
        bool reverse = false;
        for (u32 f = w.out_begin(d); f < w.out_end(d); ++f)
            reverse = reverse || w.edge_dst(f) == s;
        ok = ok && reverse;
    }
    CHECK(ok);
    CHECK(w.edge_count() >= 2 * w.node_count()); // at least the ring

    bool in_ok = true;
    u32 in_total = 0;
    for (u32 v = 0; v < w.node_count(); ++v) {
        CHECK(w.out_end(v) - w.out_begin(v) >= 2); // ring neighbours
        for (u32 k = 0; k < w.in_count(v); ++k) {
            u32 e = w.in_edge(v, k);
            in_ok = in_ok && w.edge_dst(e) == v;
            if (k > 0)
                in_ok = in_ok && w.edge_src(w.in_edge(v, k - 1)) < w.edge_src(e); // src order
        }
        in_total += w.in_count(v);
    }
    CHECK(in_ok);
    CHECK(in_total == w.edge_count());

    World one, two;
    Config c1, c2;
    CHECK(init_world(one, "world.nodes = 1\n", c1) && one.edge_count() == 0);
    CHECK(init_world(two, "world.nodes = 2\nworld.chords = 5\n", c2) && two.edge_count() == 2);
}

void test_conservation_and_flow() {
    Run run;
    CHECK(make_run(run, kBusyWorld, 4));
    const u64 units = run.world().total_units();
    bool conserved = true, all_delivered = true, codons_ok = true;
    u64 refused = 0, bounced = 0, sent = 0;
    for (int t = 0; t < 200; ++t) {
        run.step();
        const TickStats& s = run.world().last_tick();
        conserved = conserved && run.world().total_units() == units;
        all_delivered = all_delivered && s.parcels_delivered == s.parcels_sent;
        refused += s.parcels_refused;
        bounced += s.units_bounced;
        sent += s.parcels_sent;
    }
    for (u32 n = 0; n < run.world().node_count(); ++n)
        for (u32 i = 0; i < run.world().soup_size(); ++i)
            codons_ok = codons_ok && run.world().soup(n)[i] < 64;
    CHECK(conserved);
    CHECK(all_delivered); // no latency yet: every parcel lands the tick it is sent
    CHECK(codons_ok);
    CHECK(sent > 0 && refused > 0 && bounced > 0);
    CHECK(run.world().tick() == 200);
}

void test_knobs() {
    Run run;
    CHECK(make_run(run, kBusyWorld, 1));
    World& w = run.world();
    CHECK(w.check_knob("demo.send_rate", "0.3") == KnobResult::ok);
    CHECK(w.check_knob("demo.send_rate", "2") == KnobResult::bad_value);
    CHECK(w.check_knob("demo.send_rate", "lots") == KnobResult::bad_value);
    CHECK(w.check_knob("world.nodes", "10") == KnobResult::not_live);
    CHECK(w.check_knob("seed", "1") == KnobResult::not_live);
    CHECK(w.check_knob("demo.nope", "1") == KnobResult::unknown_key);

    CHECK(w.set_knob("demo.send_rate", "0") == KnobResult::ok);
    run.step();
    CHECK(w.last_tick().parcels_sent == 0); // the knob took effect
    CHECK(w.set_knob("demo.mutations_per_tick", "0") == KnobResult::ok);
    u64 h = w.hash();
    run.step();
    CHECK(w.last_tick().mutations == 0);
    // Nothing moves and nothing mutates, so only the tick number changes.
    CHECK(w.hash() != h);
}

void test_hash_basics() {
    World a, b, c;
    Config ca, cb, cc;
    CHECK(init_world(a, kBusyWorld, ca));
    CHECK(init_world(b, kBusyWorld, cb));
    CHECK(init_world(
        c, "seed = 43\nworld.nodes = 200\nworld.chords = 100\nworld.soup_size = 1024\n", cc));
    CHECK_EQ_U64(a.hash(), b.hash());
    CHECK(a.hash() != c.hash());
}

} // namespace

void suite_world() {
    test_defaults_and_validation();
    test_graph();
    test_conservation_and_flow();
    test_knobs();
    test_hash_basics();
}
