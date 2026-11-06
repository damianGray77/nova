#include "nova/world.h"

#include <cmath>  // floor
#include <cstdio> // snprintf
#include <cstring> // strlen, memcmp

#include "nova/hash.h"
#include "nova/rng.h"

#include "warp_layout.inc" // the warp image's layout (Phase 8)

namespace nova {

namespace {

// The live knobs. Everything else World::init reads is structural.
enum class Knob {
    none,
    send_rate,
    sends_per_tick,
    mutations_per_tick,
    max_parcel,
    spark_rate,
    spark_grant,
    spark_min,
    spark_max,
    // Phase 6 (design.md 6.12): one number for every node, nothing to allocate
    cpu_wear,
    heat_ambient,
    edge_corruption
};

struct KnobValue {
    Knob knob = Knob::none;
    double f = 0;
    u32 u = 0;
};

Knob knob_of(Str key, Model model) {
    if (model == Model::soup) {
        if (key.eq("spark.rate"))
            return Knob::spark_rate;
        if (key.eq("spark.grant")) // Phase 1b D4: spark settings are live knobs
            return Knob::spark_grant;
        if (key.eq("spark.min_len"))
            return Knob::spark_min;
        if (key.eq("spark.max_len"))
            return Knob::spark_max;
        if (key.eq("cpu.wear"))
            return Knob::cpu_wear;
        if (key.eq("heat.ambient"))
            return Knob::heat_ambient;
        if (key.eq("edge.corruption"))
            return Knob::edge_corruption;
        return Knob::none;
    }
    if (key.eq("demo.send_rate"))
        return Knob::send_rate;
    if (key.eq("demo.sends_per_tick"))
        return Knob::sends_per_tick;
    if (key.eq("demo.mutations_per_tick"))
        return Knob::mutations_per_tick;
    if (key.eq("demo.max_parcel"))
        return Knob::max_parcel;
    return Knob::none;
}

bool is_structural(Str key) {
    const char* const prefixes[] = {"world.",  "isa.",    "cpu.",   "heat.", "hazard.", "contact.", "tierra.",
                                    "spark.", "seed.",   "mem.",   "detect.", "gpu.",  "npu.",     "edge.",
                                    "vary.",  "disk.",   "salvage.", "signal.", "node."};
    if (key.eq("seed"))
        return true;
    for (const char* p : prefixes)
        if (key.starts_with(p))
            return true;
    return false;
}

bool parse_u32_in(Str v, u32 lo, u32 hi, u32* out) {
    u64 x;
    if (!parse_u64(v, &x) || x < lo || x > hi)
        return false;
    *out = static_cast<u32>(x);
    return true;
}

KnobResult parse_knob(Str key, Str v, Model model, u32 soup_size, bool dispersal, KnobValue* out) {
    out->knob = knob_of(key, model);
    switch (out->knob) {
    case Knob::cpu_wear:
    case Knob::edge_corruption:
        // edge.corruption acts on cargo: a world without dispersal has none,
        // and never read the key.
        if (out->knob == Knob::edge_corruption && !dispersal)
            return KnobResult::not_live;
        if (!parse_f64(v, &out->f) || !(out->f >= 0.0 && out->f <= 1.0))
            return KnobResult::bad_value;
        return KnobResult::ok;
    case Knob::heat_ambient:
        if (!parse_f64(v, &out->f) || !(out->f >= 0.0 && out->f <= 1e6))
            return KnobResult::bad_value;
        return KnobResult::ok;
    case Knob::none:
        return is_structural(key) ? KnobResult::not_live : KnobResult::unknown_key;
    case Knob::send_rate:
        if (!parse_f64(v, &out->f) || !(out->f >= 0.0 && out->f <= 1.0))
            return KnobResult::bad_value;
        return KnobResult::ok;
    case Knob::sends_per_tick:
        return parse_u32_in(v, 0, 1024, &out->u) ? KnobResult::ok : KnobResult::bad_value;
    case Knob::mutations_per_tick:
        return parse_u32_in(v, 0, 1000000, &out->u) ? KnobResult::ok : KnobResult::bad_value;
    case Knob::max_parcel:
        return parse_u32_in(v, 1, 1000000000, &out->u) ? KnobResult::ok : KnobResult::bad_value;
    case Knob::spark_rate:
        if (!parse_f64(v, &out->f) || !(out->f >= 0.0 && out->f <= 1e6))
            return KnobResult::bad_value;
        return KnobResult::ok;
    case Knob::spark_grant:
        return parse_u32_in(v, 1, 10000000, &out->u) ? KnobResult::ok : KnobResult::bad_value;
    case Knob::spark_min:
    case Knob::spark_max:
        // Each length is checked on its own; a min above the max (for a while,
        // between two changes) is read as the range between them.
        return parse_u32_in(v, 1, soup_size, &out->u) ? KnobResult::ok : KnobResult::bad_value;
    }
    return KnobResult::unknown_key;
}

const char* const kKnobKeys[] = {"demo.send_rate", "demo.sends_per_tick", "demo.mutations_per_tick",
                                 "demo.max_parcel"};
const char* const kKnobDefaults[] = {"0.5", "4", "4", "100"};

// Reads a structural u32 key with a range check, reporting through cfg.
u32 get_u32_in(Config& cfg, Str key, u64 def, u64 lo, u64 hi) {
    u64 v = cfg.get_u64(key, def);
    if (v < lo || v > hi) {
        char what[96];
        snprintf(what, sizeof what, "must be between %llu and %llu",
                 static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));
        cfg.fail(key, what);
        return static_cast<u32>(lo);
    }
    return static_cast<u32>(v);
}

double get_f64_in(Config& cfg, Str key, double def, double lo, double hi) {
    double v = cfg.get_f64(key, def);
    if (!(v >= lo && v <= hi)) {
        char what[96];
        snprintf(what, sizeof what, "must be between %g and %g", lo, hi);
        cfg.fail(key, what);
        return lo;
    }
    return v;
}

// The vary.* key for each noise field (kFieldRugged has none).
// (kFieldLand has none: world.land and world.drift drive it.)
const char* const kVaryKeys[kFieldCount] = {nullptr,          "vary.soup_size", "vary.millicores", "vary.gpus",
                                            "vary.npus",      "vary.cool",      "vary.spark_rate", "vary.radiation",
                                            "vary.disk",      "vary.volatility", "vary.endurance", "vary.noise",
                                            nullptr};

u32 scaled_u32(u32 mean, double f, u32 lo, u32 hi) {
    double v = floor(static_cast<double>(mean) * f + 0.5);
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return static_cast<u32>(v);
}

double clamp_f(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

const char* describe(KnobResult r) {
    switch (r) {
    case KnobResult::ok:
        return "ok";
    case KnobResult::unknown_key:
        return "unknown knob";
    case KnobResult::not_live:
        return "structural setting, cannot change mid-run";
    case KnobResult::bad_value:
        return "value out of range or malformed";
    }
    return "?";
}

bool World::init(Config& cfg) {
    seed_ = cfg.get_u64("seed", 1);
    nodes_ = get_u32_in(cfg, "world.nodes", 64, 1, 1000000);
    Str graph = cfg.get_str("world.graph", "ring");
    procedural_ = graph.eq("procedural");
    landscape_ = graph.eq("landscape");
    spatial_ = procedural_ || landscape_;
    if (!spatial_ && !graph.eq("ring"))
        cfg.fail("world.graph", "must be ring, procedural or landscape");
    u32 chords = spatial_ ? 0 : get_u32_in(cfg, "world.chords", nodes_ / 2, 0, 10000000);
    GraphParams gp;
    gp.nodes = nodes_;
    if (procedural_) {
        gp.links = get_u32_in(cfg, "world.links", 4, 1, 16);
        gp.ruggedness = get_f64_in(cfg, "world.ruggedness", 0.2, 0.0, 0.9);
        gp.long_links = get_f64_in(cfg, "world.long_links", 0.02, 0.0, 1.0);
        gp.latency_scale = get_f64_in(cfg, "edge.latency_scale", 20.0, 0.0, 1e6);
        gp.bandwidth = get_u32_in(cfg, "edge.bandwidth", 32, 1, 1u << 24);
    }
    if (landscape_) // Phase 7 (design.md 6.13): one world generated like a game level
        read_land_params(cfg);
    soup_size_ = get_u32_in(cfg, "world.soup_size", 256, 1, 1u << 24);
    Str model = cfg.get_str("world.model", "demo");
    if (model.eq("demo"))
        model_ = Model::demo;
    else if (model.eq("soup"))
        model_ = Model::soup;
    else
        cfg.fail("world.model", "must be demo or soup");
    u32 pool_initial = 0;
    if (model_ == Model::demo) {
        pool_capacity_ = get_u32_in(cfg, "world.pool_capacity", 1000, 1, 1000000000);
        pool_initial = get_u32_in(cfg, "world.pool_initial", pool_capacity_ / 2, 0, pool_capacity_);
    }
    outbox_capacity_ = get_u32_in(cfg, "world.outbox_capacity", 2, 1, 1024);

    if (model_ == Model::demo) {
        for (usize i = 0; i < sizeof kKnobKeys / sizeof kKnobKeys[0]; ++i) {
            Str key = kKnobKeys[i];
            Str v = cfg.get_str(key, kKnobDefaults[i]);
            KnobResult r = set_knob(key, v);
            if (r != KnobResult::ok)
                cfg.fail(key, describe(r));
        }
    }
    // Phase 3 node keys (soup model).
    double vary[kFieldCount] = {};
    bool one_origin = false;
    u32 seed_node = kNone;
    dispersal_ = false;
    lifecycle_ = false;
    accel_async_ = false;
    channels_ = 0;
    edge_corruption_ = edge_failure_ = 0;
    if (model_ == Model::soup) {
        soup_params_from_config(cfg, soup_size_, &soup_params_);
        detect_params_from_config(cfg, &detect_);
        dispersal_ = soup_params_.dispersal;
        accel_async_ = soup_params_.accel_async;
        accel_open(cfg);
        warp_open(cfg);
        // In a landscape the resources and sparks follow their distributions
        // (res.*, spark.*) instead of vary.*; the other properties still vary.
        if (spatial_)
            for (u32 f = 1; f < kFieldCount; ++f)
                if (kVaryKeys[f] && (procedural_ || !resource_field(f)))
                    vary[f] = get_f64_in(cfg, kVaryKeys[f], 0.0, 0.0, 1.0);
        if (landscape_)
            read_spreads(cfg);
        Str origins = cfg.get_str("world.origins", "all");
        one_origin = origins.eq("one");
        if (!one_origin && !origins.eq("all"))
            cfg.fail("world.origins", "must be all or one");
        Str sn = cfg.get_str("seed.node", "all");
        if (!sn.eq("all")) {
            u64 v = 0;
            if (!parse_u64(sn, &v) || v >= nodes_)
                cfg.fail("seed.node", "must be all or a node id below world.nodes");
            else
                seed_node = static_cast<u32>(v);
        }
        edge_corruption_ = edge_failure_ = 0;
        edge_downtime_ = 1000;
        // Phase 4: the lifecycle's world-level settings.
        land_ = 1.0;
        drift_ = 0.0;
        recover_every_ = 100;
        fallow_min_ = 2000;
        u32 budget = 0;
        if (spatial_) {
            if (procedural_) { // a landscape's land is its own, and doesn't drift
                land_ = get_f64_in(cfg, "world.land", 0.8, 0.01, 1.0); // Phase 10: every feature on by default
                drift_ = get_f64_in(cfg, "world.drift", 0.00001, 0.0, 1.0);
            }
            budget = get_u32_in(cfg, "world.budget", 0, 0, nodes_);
            recover_every_ = get_u32_in(cfg, "node.recover_every", 100, 1, 1000000000);
            fallow_min_ = cfg.get_u64("node.fallow_min", 2000);
        }
        lifecycle_ = soup_params_.endurance > 0 || soup_params_.starve_ticks > 0 || land_ < 1.0;
        if (lifecycle_ && !spatial_)
            cfg.fail("node.endurance", "the node lifecycle needs world.graph = procedural or landscape");
        budget_ = budget;
        // Phase 4: the signal channel's world-level settings.
        channels_ = soup_params_.signal_channels;
        attenuation_ = 0.25;
        reach_ = 8;
        if (channels_) {
            attenuation_ = get_f64_in(cfg, "signal.attenuation", 0.25, 0.0, 1.0);
            reach_ = get_u32_in(cfg, "signal.reach", 8, 0, 64);
        }
        if (dispersal_) {
            edge_capacity_ = get_u32_in(cfg, "edge.capacity", 2, 1, 1024);
            if (!spatial_)
                gp.bandwidth = get_u32_in(cfg, "edge.bandwidth", 32, 1, 1u << 24);
            // Phase 4 (design.md 6.10): transit corruption and link failures.
            edge_corruption_ = get_f64_in(cfg, "edge.corruption", 0.0002, 0.0, 1.0);
            edge_failure_ = get_f64_in(cfg, "edge.failure", 0.0001, 0.0, 1.0);
            edge_downtime_ = get_f64_in(cfg, "edge.downtime", 1000.0, 1.0, 1e9);
        }
    }
    if (landscape_ && model_ != Model::soup)
        cfg.fail("world.graph", "a landscape needs world.model = soup");
    // Phase 7b: the disk tier on real disk, in a folder of this run's own
    // under disk.path (its files go when the run ends).
    disk_folder_.clear();
    if (model_ == Model::soup) {
        const Str dp = cfg.get_str("disk.path", "");
        if (dp.n && soup_params_.disk > 0) {
            static u32 runs = 0;
            disk_folder_.append(dp);
            disk_folder_.append("\\nova-");
            disk_folder_.append_u64(process_id());
            disk_folder_.append_char('-');
            disk_folder_.append_u64(++runs);
            if (!make_folder(disk_folder_.c_str())) {
                disk_folder_.clear();
                cfg.fail("disk.path", "cannot create a folder there (does it exist?)");
            }
        }
    }
    if (!cfg.ok())
        return false;

    // --- graph --------------------------------------------------------------
    links_cut_ = long_added_ = 0;
    latency_scale_ = procedural_ ? gp.latency_scale : 0.0;
    gp_ = gp;
    edge_bridge_.clear();
    edge_phase_.clear();
    bridge_list_.clear();
    if (landscape_) {
        TextBuf why;
        if (!generate_landscape(seed_, lp_, &ls_, &why)) {
            cfg.fail("world.nodes", why.c_str());
            return false;
        }
        if (!build_edges(ls_.keys.data(), ls_.keys.size()))
            NOVA_FATAL("World::init: generated landscape failed validation");
        node_x_.assign(ls_.x.data(), ls_.x.size());
        node_y_.assign(ls_.y.data(), ls_.y.size());
        edge_latency_.assign(ls_.latency.data(), ls_.latency.size());
        edge_bandwidth_.assign(ls_.bandwidth.data(), ls_.bandwidth.size());
        edge_dist_.assign(ls_.dist.data(), ls_.dist.size());
        edge_bridge_.assign(ls_.bridge.data(), ls_.bridge.size());
        edge_phase_.assign(ls_.phase.data(), ls_.phase.size());
        for (u32 e = 0; e < edge_bridge_.size(); ++e)
            if (edge_bridge_[e])
                bridge_list_.push(e);
        long_added_ = ls_.long_added;
    } else if (procedural_) {
        ProcGraph& pg = pg_; // kept: the lifecycle rebuilds from it
        generate_graph(seed_, gp, &pg);
        if (!build_edges(pg.keys.data(), pg.keys.size()))
            NOVA_FATAL("World::init: generated graph failed validation");
        node_x_.assign(pg.x.data(), pg.x.size());
        node_y_.assign(pg.y.data(), pg.y.size());
        edge_latency_.assign(pg.latency.data(), pg.latency.size());
        edge_bandwidth_.assign(pg.bandwidth.data(), pg.bandwidth.size());
        edge_dist_.assign(pg.dist.data(), pg.dist.size());
        links_cut_ = pg.cut;
        long_added_ = pg.long_added;
    } else {
        // A ring plus random chords, from the world-level stream.
        Array<u64> keys;
        auto link = [&keys](u32 a, u32 b) {
            keys.push((static_cast<u64>(a) << 32) | b);
            keys.push((static_cast<u64>(b) << 32) | a);
        };
        if (nodes_ == 2) {
            link(0, 1);
        } else if (nodes_ >= 3) {
            for (u32 i = 0; i < nodes_; ++i)
                link(i, (i + 1) % nodes_);
        }
        Rng g(seed_, Rng::kWorldNode, 0);
        for (u32 c = 0; c < chords && nodes_ >= 2; ++c) {
            u32 a = static_cast<u32>(g.below(nodes_));
            u32 b = static_cast<u32>(g.below(nodes_));
            if (a != b)
                link(a, b);
        }
        sort_u64(keys.data(), keys.size());
        usize unique = 0;
        for (usize i = 0; i < keys.size(); ++i)
            if (unique == 0 || keys[i] != keys[unique - 1])
                keys[unique++] = keys[i];
        if (!build_edges(keys.data(), unique))
            NOVA_FATAL("World::init: generated graph failed validation");
        node_x_.clear();
        node_y_.clear();
        edge_dist_.clear();
        // Ring edges have no length: latency 1, full bandwidth (dispersal only).
        edge_latency_.resize(unique);
        edge_latency_.fill(1);
        edge_bandwidth_.resize(unique);
        edge_bandwidth_.fill(gp.bandwidth);
    }

    // --- node state ---------------------------------------------------------
    tick_ = 0;
    last_ = {};
    soup_last_ = {};
    cargo_.clear();
    cargo_codons_.clear();
    next_seq_ = 0;
    acc_ops_.clear();
    acc_in_.clear();
    acc_addr_.clear();
    acc_job_.clear();
    acc_slot_.clear();
    acc_held_.clear();
    acc_held_scalar_.clear();
    acc_held_flags_.clear();
    down_until_.resize(edge_src_.size());
    down_until_.fill(0);
    live_.clear();
    born_.clear();
    fallow_since_.clear();
    life_.clear();
    lived_.clear();
    wevents_.clear();
    collapses_total_ = recoveries_total_ = world_collapses_ = world_recoveries_ = 0;
    bool fallow_at_start = false;
    if (model_ == Model::soup && lifecycle_) {
        // Sites: the land field decides which are nodes at the start.
        land_at(0, &live_);
        born_.resize(nodes_);
        fallow_since_.resize(nodes_);
        life_.resize(nodes_);
        lived_.assign(live_.data(), live_.size());
        u32 alive = 0;
        for (u32 n = 0; n < nodes_; ++n)
            alive += live_[n];
        fallow_at_start = alive < nodes_;
        if (budget_ == 0)
            budget_ = alive; // spawn tracks collapse
    }
    if (model_ == Model::soup) {
        delete[] soups_;
        delete[] lineages_;
        lineages_ = nullptr;
        init_nodes(vary, one_origin, seed_node);
        soups_ = new Soup[nodes_];
        if (disk_folder_.size())
            for (u32 n = 0; n < nodes_; ++n)
                soups_[n].set_disk_folder(disk_folder_.c_str());
        for (u32 n = 0; n < nodes_; ++n)
            soups_[n].init(&node_params_[n], seed_, n);
        // Signals (Phase 4): the pending ring outlasts the slowest path any
        // graph of these sites could give (a hop's latency is at most
        // 1 + floor(sqrt 2 x latency_scale)).
        pending_.clear();
        sig_slots_ = 0;
        if (channels_) {
            u32 max_lat = 1 + static_cast<u32>(floor(1.4143 * latency_scale_));
            if (landscape_) { // fixed paths: the slowest land path (signals don't cross bridges)
                max_lat = 1;
                for (u32 e = 0; e < edge_count(); ++e)
                    if (!edge_bridge_[e] && edge_latency_[e] > max_lat)
                        max_lat = edge_latency_[e];
            }
            sig_slots_ = 2 + reach_ * max_lat;
            pending_.resize(static_cast<usize>(sig_slots_) * nodes_ * channels_);
            att_pow_.resize(static_cast<usize>(reach_) + 1);
            double a = 1.0;
            for (u32 h = 0; h <= reach_; ++h, a *= attenuation_)
                att_pow_[h] = a;
        }
        if (fallow_at_start)
            rebuild_graph(); // edges among the live sites only (calls init_ports)
        else
            init_ports();
        if (detect_.enabled) {
            lineages_ = new Lineages[nodes_];
            for (u32 n = 0; n < nodes_; ++n) {
                lineages_[n].init(&detect_, n);
                soups_[n].set_observer(&lineages_[n]);
            }
        }
        return true;
    }
    soup_.resize(static_cast<usize>(nodes_) * soup_size_);
    pool_.resize(nodes_);
    rng_counter_.resize(nodes_);
    tally_.resize(nodes_);
    for (u32 n = 0; n < nodes_; ++n) {
        Rng r(seed_, n, Rng::kInitTick);
        u8* s = soup_.data() + static_cast<usize>(n) * soup_size_;
        for (u32 i = 0; i < soup_size_; ++i)
            s[i] = static_cast<u8>(r.next() & 63); // 64 divides 2^64: unbiased
        pool_[n] = pool_initial;
    }
    last_ = {};
    return true;
}

bool World::build_edges(const u64* keys, usize n) {
    if (n > 0xFFFFFFFFull)
        return false;
    for (usize i = 0; i < n; ++i) {
        u32 src = static_cast<u32>(keys[i] >> 32);
        u32 dst = static_cast<u32>(keys[i]);
        if (src >= nodes_ || dst >= nodes_ || src == dst)
            return false;
        if (i > 0 && keys[i] <= keys[i - 1])
            return false;
    }
    edge_src_.resize(n);
    edge_dst_.resize(n);
    out_begin_.resize(static_cast<usize>(nodes_) + 1);
    in_begin_.resize(static_cast<usize>(nodes_) + 1);
    in_list_.resize(n);
    out_begin_.fill(0);
    in_begin_.fill(0);
    for (usize i = 0; i < n; ++i) {
        edge_src_[i] = static_cast<u32>(keys[i] >> 32);
        edge_dst_[i] = static_cast<u32>(keys[i]);
        ++out_begin_[edge_src_[i] + 1];
        ++in_begin_[edge_dst_[i] + 1];
    }
    for (u32 v = 0; v < nodes_; ++v) {
        out_begin_[v + 1] += out_begin_[v];
        in_begin_[v + 1] += in_begin_[v];
    }
    // Edges are sorted by src, so filling in edge order leaves every dst group
    // sorted by src -- the fixed delivery order.
    Array<u32> fill;
    fill.assign(in_begin_.data(), nodes_);
    for (usize e = 0; e < n; ++e)
        in_list_[fill[edge_dst_[e]]++] = static_cast<u32>(e);

    outbox_.resize(n * outbox_capacity_);
    outbox_count_.resize(n);
    outbox_count_.fill(0);
    return true;
}

// Each node's own SoupParams (Phase 3, design.md 6.9). A copy of the configured
// ones, then: the vary.* factors 1 + f x noise from each property's field (a
// procedural world only), sparks only on node 0 with world.origins = one, the
// seed only on seed.node, and the size-bound settings clamped to the node's
// size. With nothing varied every node's copy is the configured one exactly.
void World::init_nodes(const double* vary, bool one_origin, u32 seed_node) {
    for (u32 f = 0; f < kFieldCount; ++f)
        vary_[f] = vary[f];
    one_origin_ = one_origin;
    seed_node_ = seed_node;
    node_params_.resize(nodes_);
    spark_factor_.resize(nodes_);
    if (landscape_)
        spread_resources();
    Array<double> z[kFieldCount];
    fields_at(0, z);
    for (u32 n = 0; n < nodes_; ++n)
        derive_node(n, z, true);
}

// The property fields at a tick: static (2D) unless world.drift gives them a
// time axis (Phase 4 lifecycle), each rank-normalised over every site.
void World::fields_at(u64 tick, Array<double>* z) {
    for (u32 f = 1; f < kFieldCount; ++f) {
        z[f].clear();
        if (!spatial_ || vary_[f] <= 0)
            continue;
        if (drift_ > 0)
            node_field3(seed_, f, node_x_, node_y_, static_cast<double>(tick) * drift_, &z[f]);
        else
            node_field(seed_, f, node_x_, node_y_, &z[f]);
    }
}

void World::derive_node(u32 n, const Array<double>* z, bool first) {
    const SoupParams& m = soup_params_;
    auto factor = [&](u32 f) { return z[f].size() ? 1.0 + vary_[f] * z[f][n] : 1.0; };
    SoupParams& p = node_params_[n];
    p = m;
    if (z[kFieldSoupSize].size())
        p.size = scaled_u32(m.size, factor(kFieldSoupSize), 64 < m.size ? 64 : m.size, 1u << 24);
    if (z[kFieldMillicores].size())
        p.millicores = scaled_u32(m.millicores, factor(kFieldMillicores), 1, 10000000);
    if (z[kFieldGpus].size() && m.gpus)
        p.gpus = scaled_u32(m.gpus, factor(kFieldGpus), 1, 10000000);
    if (z[kFieldNpus].size() && m.npus)
        p.npus = scaled_u32(m.npus, factor(kFieldNpus), 1, 10000000);
    if (z[kFieldCool].size())
        p.cool = static_cast<float>(clamp_f(m.cool * factor(kFieldCool), 0.0001, 1.0));
    if (z[kFieldRadiation].size()) {
        const double r = factor(kFieldRadiation);
        p.copy_rate = clamp_f(m.copy_rate * r, 0, 1);
        p.flaw_rate = clamp_f(m.flaw_rate * r, 0, 1);
        p.rot_rate = clamp_f(m.rot_rate * r, 0, 1);
    }
    // Phase 4 properties.
    if (z[kFieldDisk].size() && m.disk)
        p.disk = scaled_u32(m.disk, factor(kFieldDisk), 64 < m.disk ? 64 : m.disk, 1u << 24);
    if (z[kFieldVolatility].size())
        p.volatility = clamp_f(m.volatility * factor(kFieldVolatility), 0, 1);
    if (z[kFieldNoise].size())
        p.signal_floor = m.signal_floor * factor(kFieldNoise);
    if (z[kFieldEndurance].size() && m.endurance > 0)
        p.endurance = m.endurance * factor(kFieldEndurance);
    double sf = factor(kFieldSparkRate);
    if (landscape_) {
        // The site's shares (Phase 7, C6): fixed for every life of the site.
        p.size = share_mem_[n];
        p.millicores = share_cpu_[n];
        p.gpus = m.gpus ? share_gpu_[n] : 0;
        p.npus = m.npus ? share_npu_[n] : 0;
        p.disk = m.disk ? share_disk_[n] : 0;
        if (!ecpu_w_.empty()) {
            const double e = m.e_share * ecpu_w_[n];
            p.e_share = e < 1.0 ? e : 1.0;
        }
        sf = spark_w_[n];
        p.spark_initial = static_cast<u32>(floor(m.spark_initial * sf + 0.5));
    }
    if (one_origin_ && n != 0) {
        sf = 0;
        p.spark_initial = 0;
    }
    spark_factor_[n] = sf;
    p.spark_rate = m.spark_rate * sf;
    // Settings bounded by the soup size (checked against the mean).
    p.search_limit = min_of(p.search_limit, p.size);
    p.contact_range = min_of(p.contact_range, p.size);
    p.spark_min = min_of(p.spark_min, p.size);
    p.spark_max = min_of(p.spark_max, p.size);
    if (seed_node_ != kNone && n != seed_node_)
        p.seed_len = 0;
    if (p.seed_len && (p.seed_at >= p.size || p.seed_len > p.size - p.seed_at))
        p.seed_len = 0; // no room on this node
    // The lifecycle: a site that starts as sea gets nothing until it appears;
    // a later life is a fresh primordial soup, never seeded.
    if ((first && !live_.empty() && !live_[n]) || !first) {
        p.seed_len = 0;
        if (first)
            p.spark_initial = 0;
    }
}

// Neighbours are numbered nearest first by hidden distance (ties: lower id),
// which in a ring world is id order. Each soup gets its slice.
void World::init_ports() {
    const u32 e = edge_count();
    port_list_.resize(e);
    // A landscape numbers its land paths first and its bridges after them, so
    // a bridge never shifts a land neighbour's number (Phase 7, C4).
    auto rank = [this](u32 i) {
        return (landscape_ && edge_bridge_[i] ? 1000.0 : 0.0) + (spatial_ ? edge_dist_[i] : 0.0);
    };
    for (u32 n = 0; n < nodes_; ++n) {
        const u32 b = out_begin_[n], end = out_begin_[n + 1];
        for (u32 i = b; i < end; ++i) { // insertion sort; edges arrive in dst order
            u32 at = i;
            const double d = rank(i);
            while (at > b && rank(port_list_[at - 1]) > d) {
                port_list_[at] = port_list_[at - 1];
                --at;
            }
            port_list_[at] = i;
        }
    }
    in_flight_.resize(e);
    in_flight_.fill(0);
    for (usize i = 0; i < cargo_.size(); ++i)
        ++in_flight_[cargo_[i].edge];
    // Phase 4: each edge's opposite, and which are down (down_until_ is world
    // state, kept by its owner; this derives the flags the ports read).
    edge_rev_.resize(e);
    for (u32 i = 0; i < e; ++i) {
        const u64 want = (static_cast<u64>(edge_dst_[i]) << 32) | edge_src_[i];
        u32 lo = out_begin_[edge_dst_[i]], hi = out_begin_[edge_dst_[i] + 1];
        while (lo < hi) { // the dst's out-edges are sorted by their dst
            const u32 mid = lo + (hi - lo) / 2;
            const u64 k = (static_cast<u64>(edge_src_[mid]) << 32) | edge_dst_[mid];
            if (k < want)
                lo = mid + 1;
            else
                hi = mid;
        }
        edge_rev_[i] = lo;
    }
    down_until_.resize(e);
    link_down_.resize(e);
    for (u32 i = 0; i < e; ++i)
        link_down_[i] = down_until_[i] != 0;
    if (landscape_) {
        port_down_.resize(e);
        shut_.resize(e);
        refresh_ports();
    }
    for (u32 n = 0; n < nodes_; ++n) {
        DispersalPorts P;
        if (dispersal_) {
            P.edges = port_list_.data() + out_begin_[n];
            P.in_flight = in_flight_.data();
            P.down = landscape_ ? port_down_.data() : (edge_failure_ > 0 ? link_down_.data() : nullptr);
            P.degree = out_begin_[n + 1] - out_begin_[n];
            P.capacity = edge_capacity_;
        }
        soups_[n].set_ports(P);
    }
    if (channels_)
        compute_routes();
}

// A landscape's ports (Phase 7, C4-C5): a path refuses departures while it has
// failed, while it is a shut bridge, and while either end is a fallow site.
// Bridges open and shut on their schedule, for the tick about to run.
void World::refresh_ports() {
    const u32 e = edge_count();
    for (u32 i = 0; i < e; ++i) {
        shut_[i] = edge_bridge_[i] && !bridge_open(tick_, lp_.open_every, lp_.open_ticks, edge_phase_[i]) ? 1 : 0;
        const bool fallow = !live_.empty() && (!live_[edge_src_[i]] || !live_[edge_dst_[i]]);
        port_down_[i] = link_down_[i] || shut_[i] || fallow ? 1 : 0;
    }
    ports_dirty_ = false;
}

void World::bridges_update() {
    bool changed = ports_dirty_;
    for (usize k = 0; k < bridge_list_.size() && !changed; ++k) {
        const u32 i = bridge_list_[k];
        changed = shut_[i] != (bridge_open(tick_, lp_.open_every, lp_.open_ticks, edge_phase_[i]) ? 0 : 1);
    }
    if (changed)
        refresh_ports();
}

bool World::edge_shut(u32 e) const { return landscape_ && e < shut_.size() && shut_[e]; }

// The landscape's settings (plan C1-C4).
void World::read_land_params(Config& cfg) {
    LandParams& p = lp_;
    p = LandParams{};
    p.nodes = nodes_;
    p.aspect = get_f64_in(cfg, "land.aspect", 1.6, 0.1, 10.0);
    p.fraction = get_f64_in(cfg, "land.fraction", 0.45, 0.01, 1.0);
    p.scale = get_f64_in(cfg, "land.scale", 2.5, 0.1, 100.0);
    p.warp = get_f64_in(cfg, "land.warp", 0.3, 0.0, 2.0);
    p.border = get_f64_in(cfg, "land.border", 0.6, 0.0, 4.0);
    p.resolution = get_u32_in(cfg, "land.resolution", 256, 16, 8192);
    p.paths_mean = get_f64_in(cfg, "paths.mean", 3.0, 0.0, 64.0);
    p.paths_spread = get_f64_in(cfg, "paths.spread", 0.5, 0.0, 4.0);
    p.paths_isolated = get_f64_in(cfg, "paths.isolated", 0.01, 0.0, 1.0);
    p.paths_hubs = get_f64_in(cfg, "paths.hubs", 0.02, 0.0, 1.0);
    p.paths_max = get_u32_in(cfg, "paths.max", 12, 1, 64);
    p.paths_connected = cfg.get_bool("paths.connected", false);
    p.paths_long = get_f64_in(cfg, "paths.long", 0.01, 0.0, 1.0);
    p.latency_scale = get_f64_in(cfg, "edge.latency_scale", 20.0, 0.0, 1e6);
    p.bandwidth = get_u32_in(cfg, "edge.bandwidth", 32, 1, 1u << 24);
    p.bridge_count = get_u32_in(cfg, "bridge.count", 1, 0, 64);
    p.bridge_isolated = get_f64_in(cfg, "bridge.isolated", 0.0, 0.0, 1.0);
    p.bridge_max_length = get_f64_in(cfg, "bridge.max_length", 0.25, 0.0, 100.0);
    p.bridge_latency_scale = get_f64_in(cfg, "bridge.latency_scale", 100.0, 0.0, 1e6);
    p.bridge_bandwidth = get_u32_in(cfg, "bridge.bandwidth", 8, 1, 1u << 24);
    p.open_every = get_u32_in(cfg, "bridge.open_every", 20000, 0, 1000000000);
    p.open_ticks = get_u32_in(cfg, "bridge.open_ticks", 1000, 0, 1000000000);
    const Str ph = cfg.get_str("bridge.phase", "staggered");
    if (ph.eq("together"))
        p.phase = kPhaseTogether;
    else if (ph.eq("staggered"))
        p.phase = kPhaseStaggered;
    else if (ph.eq("random"))
        p.phase = kPhaseRandom;
    else
        cfg.fail("bridge.phase", "must be together, staggered or random");
    if (p.paths_isolated + p.paths_hubs > 1.0)
        cfg.fail("paths.hubs", "paths.isolated + paths.hubs must be at most 1");
}

// The distributions of sparks and each resource (plan C6).
void World::read_spreads(Config& cfg) {
    const char* const names[7] = {"spark", "res.memory", "res.cpu", "res.gpu", "res.npu", "res.disk", "res.ecpu"};
    for (u32 r = 0; r < 7; ++r) {
        Spread& s = spreads_[r];
        TextBuf k;
        auto key = [&](const char* leaf) -> Str {
            k.clear();
            k.append(names[r]);
            k.append_char('.');
            k.append(leaf);
            return k.view();
        };
        s.spread = get_f64_in(cfg, key("spread"), 0.0, 0.0, 4.0);
        s.smooth = get_f64_in(cfg, key("smooth"), 0.0, 0.0, 1.0);
        s.barren = get_f64_in(cfg, key("barren"), 0.0, 0.0, 1.0);
        s.hotspots = get_u32_in(cfg, key("hotspots"), 0, 0, 10000);
        s.boost = get_f64_in(cfg, key("hotspot_boost"), 2.0, 0.0, 1000.0);
        // Phase 8: rich where another distribution is poor (its noise field
        // the opposite way round), so GPU-rich regions can be CPU-poor.
        const Str a = cfg.get_str(key("against"), "none");
        const char* const others[7] = {"spark", "memory", "cpu", "gpu", "npu", "disk", "ecpu"};
        const u32 fields[7] = {kFieldSparkRate, kFieldSoupSize, kFieldMillicores, kFieldGpus,
                               kFieldNpus,      kFieldDisk,     kFieldEcpu};
        s.against = 0xFFFFFFFFu;
        bool known = a.eq("none");
        for (u32 o = 0; o < 7 && !known; ++o) {
            if (a.eq(others[o]) && o != r) {
                s.against = fields[o];
                known = true;
            }
        }
        if (!known)
            cfg.fail(key("against"), "must be none or another of spark, memory, cpu, gpu, npu, disk, ecpu");
    }
}

// vary.* fields a landscape replaces with its distributions.
bool World::resource_field(u32 f) {
    return f == kFieldSoupSize || f == kFieldMillicores || f == kFieldGpus || f == kFieldNpus ||
           f == kFieldSparkRate || f == kFieldDisk;
}

// Each site's shares of the world's totals (the configured means x nodes),
// fixed for every life of the site.
void World::spread_resources() {
    const SoupParams& m = soup_params_;
    const double W = ls_.width;
    Array<double> w;
    Array<u64> share;
    const u64 N = nodes_;
    auto split = [&](u32 idx, u32 field, u64 mean, u64 minimum, u64 maximum, Array<u32>* out) {
        spread_weights(seed_, field, spreads_[idx], node_x_, node_y_, W, &w);
        spread_total(w, mean * N, minimum, &share);
        out->resize(nodes_);
        for (u32 n = 0; n < nodes_; ++n)
            (*out)[n] = static_cast<u32>(share[n] > maximum ? maximum : share[n]);
    };
    split(1, kFieldSoupSize, m.size, 64 < m.size ? 64 : m.size, 1u << 24, &share_mem_);
    split(2, kFieldMillicores, m.millicores, 1, 10000000, &share_cpu_);
    split(3, kFieldGpus, m.gpus, 0, 10000000, &share_gpu_);
    split(4, kFieldNpus, m.npus, 0, 10000000, &share_npu_);
    split(5, kFieldDisk, m.disk, 0, 1u << 24, &share_disk_);
    spread_weights(seed_, kFieldSparkRate, spreads_[0], node_x_, node_y_, W, &spark_w_);
    ecpu_w_.clear();
    if (m.e_share > 0) // Phase 9c: E-rich and E-poor sites (res.ecpu), the share capped at all of them
        spread_weights(seed_, kFieldEcpu, spreads_[6], node_x_, node_y_, W, &ecpu_w_);
}

// Signal paths (Phase 4, design.md 6.10): from each source, a breadth-first
// search over the links that are up, out to signal.reach hops. Each target
// gets its fewest hop count and, among the fewest-hop paths, the quickest:
// delay = 1 + the latencies along it (1 for the source itself, heard next
// tick). Flooding along fewest-hop paths only means no echoes.
void World::compute_routes() {
    route_begin_.resize(static_cast<usize>(nodes_) + 1);
    route_tgt_.clear();
    route_hops_.clear();
    route_delay_.clear();
    bfs_dist_.resize(nodes_);
    bfs_delay_.resize(nodes_);
    for (u32 e = 0; e < nodes_; ++e) {
        route_begin_[e] = static_cast<u32>(route_tgt_.size());
        if (!live_.empty() && !live_[e])
            continue;
        bfs_dist_.fill(kNone);
        bfs_queue_.clear();
        bfs_dist_[e] = 0;
        bfs_delay_[e] = 1;
        bfs_queue_.push(e);
        for (usize qi = 0; qi < bfs_queue_.size(); ++qi) {
            const u32 u = bfs_queue_[qi];
            if (bfs_dist_[u] >= reach_)
                continue;
            for (u32 x = out_begin_[u]; x < out_begin_[u + 1]; ++x) {
                if (link_down_.size() && link_down_[x])
                    continue;
                const u32 v = edge_dst_[x];
                // A landscape's signals stay on land (never over a bridge) and
                // don't reach a fallow site (Phase 7).
                if (landscape_ && (edge_bridge_[x] || (!live_.empty() && !live_[v])))
                    continue;
                const u32 d = bfs_delay_[u] + edge_latency_[x];
                if (bfs_dist_[v] == kNone) {
                    bfs_dist_[v] = bfs_dist_[u] + 1;
                    bfs_delay_[v] = d;
                    bfs_queue_.push(v);
                } else if (bfs_dist_[v] == bfs_dist_[u] + 1 && d < bfs_delay_[v]) {
                    bfs_delay_[v] = d;
                }
            }
        }
        for (usize qi = 0; qi < bfs_queue_.size(); ++qi) { // in hop order
            const u32 m = bfs_queue_[qi];
            route_tgt_.push(m);
            route_hops_.push(bfs_dist_[m]);
            route_delay_.push(bfs_delay_[m]);
        }
    }
    route_begin_[nodes_] = static_cast<u32>(route_tgt_.size());
}

// The field node n hears this tick (the update phase, before its tick): what
// is pending for it now plus background noise uniform in -floor...floor from
// its own signal stream, rounded. Each node reads and clears only its own
// slot, so this runs in parallel.
void World::signal_field(u32 n) {
    const usize slot = static_cast<usize>(tick_ % sig_slots_);
    double* pend = pending_.data() + (slot * nodes_ + n) * channels_;
    i32* f = soups_[n].field_mut();
    const double fl = node_params_[n].signal_floor;
    Rng r(seed_ ^ 0x7369676E616C0000ull, n, tick_); // "signal"
    for (u32 c = 0; c < channels_; ++c) {
        double v = floor(pend[c] + fl * (2.0 * r.unit() - 1.0) + 0.5);
        v = v < -2147483648.0 ? -2147483648.0 : (v > 2147483647.0 ? 2147483647.0 : v);
        f[c] = static_cast<i32>(v);
        pend[c] = 0;
    }
}

// The barrier: each node's emission sums this tick go out along its paths,
// attenuated per hop, to arrive at tick + delay; a contribution below the
// target's floor is dropped. Serial, in source, channel and path order.
void World::scatter_signals() {
    for (u32 e = 0; e < nodes_; ++e) {
        if (!live_.empty() && !live_[e])
            continue;
        const i64* E = soups_[e].emissions();
        for (u32 c = 0; c < channels_; ++c) {
            if (E[c] == 0)
                continue;
            const double amp = static_cast<double>(E[c]);
            for (u32 i = route_begin_[e]; i < route_begin_[e + 1]; ++i) {
                const u32 m = route_tgt_[i];
                const double a = amp * att_pow_[route_hops_[i]];
                if (fabs(a) < node_params_[m].signal_floor)
                    continue;
                const usize slot = static_cast<usize>((tick_ + route_delay_[i]) % sig_slots_);
                pending_[(slot * nodes_ + m) * channels_ + c] += a;
            }
        }
    }
}

// The land mask at a tick (Phase 4 lifecycle): the top world.land fraction of
// sites by the land field (drifting with world.drift), every site at land 1.
void World::land_at(u64 tick, Array<u8>* mask) {
    mask->resize(nodes_);
    if (land_ >= 1.0) {
        mask->fill(1);
        return;
    }
    Array<double> z;
    if (drift_ > 0)
        node_field3(seed_, kFieldLand, node_x_, node_y_, static_cast<double>(tick) * drift_, &z);
    else
        node_field(seed_, kFieldLand, node_x_, node_y_, &z);
    const double keep = floor(land_ * nodes_ + 0.5);
    for (u32 n = 0; n < nodes_; ++n) {
        const double rank = nodes_ > 1 ? floor((z[n] + 1.0) * (nodes_ - 1) * 0.5 + 0.5) : 0.0;
        (*mask)[n] = rank >= nodes_ - keep ? 1 : 0;
    }
}

// The barrier (Phase 4 lifecycle, design.md 6.10). In node order, each node
// whose tick found it worn out or starved collapses: every body dies an
// ordinary death (its code stays behind as remnants), and it goes fallow.
// Then, every node.recover_every ticks, fallow sites become nodes again in
// site order while the live count is under the budget: fallow long enough,
// and land now. A returning node takes its properties from the fields as they
// are now, and its ruins weathered over the fallow time. The graph is rebuilt
// if anything changed.
void World::lifecycle_update() {
    world_collapses_ = world_recoveries_ = 0;
    bool changed = false;
    for (u32 n = 0; n < nodes_; ++n) {
        if (!live_[n] || !soups_[n].collapse_due())
            continue;
        soups_[n].collapse();
        live_[n] = 0;
        fallow_since_[n] = tick_;
        ++world_collapses_;
        wevents_.push(WorldEvent{tick_, 0, n, life_[n], 0});
        changed = true;
        if (channels_) // nothing more reaches it
            for (u32 slot = 0; slot < sig_slots_; ++slot)
                for (u32 c = 0; c < channels_; ++c)
                    pending_[(static_cast<usize>(slot) * nodes_ + n) * channels_ + c] = 0;
    }
    if ((tick_ + 1) % recover_every_ == 0) {
        u32 alive = 0;
        for (u32 n = 0; n < nodes_; ++n)
            alive += live_[n];
        if (alive < budget_) {
            Array<u8> land;
            land_at(tick_, &land);
            Array<double> z[kFieldCount];
            bool fields = false;
            for (u32 n = 0; n < nodes_ && alive < budget_; ++n) {
                if (live_[n] || !land[n] || tick_ - fallow_since_[n] < fallow_min_)
                    continue;
                if (!fields) {
                    fields_at(tick_, z);
                    fields = true;
                }
                Ruins ruins;
                if (lived_[n])
                    soups_[n].take_ruins(&ruins, tick_ - fallow_since_[n]);
                ++life_[n];
                derive_node(n, z, false);
                soups_[n].init(&node_params_[n], seed_, n, life_[n], lived_[n] ? &ruins : nullptr);
                if (lineages_) {
                    lineages_[n].new_life();
                    soups_[n].set_observer(&lineages_[n]);
                }
                live_[n] = 1;
                lived_[n] = 1;
                born_[n] = tick_;
                ++alive;
                ++world_recoveries_;
                wevents_.push(WorldEvent{tick_, 1, n, life_[n], 0});
                changed = true;
            }
        }
    }
    collapses_total_ += world_collapses_;
    recoveries_total_ += world_recoveries_;
    if (!changed)
        return;
    if (!landscape_) {
        rebuild_graph();
        return;
    }
    // A landscape's paths are fixed (Phase 7, C5): a collapsed site's paths go
    // down with whatever they carry, and come back with it.
    if (world_collapses_) {
        drop_scratch_.resize(edge_count());
        drop_scratch_.fill(0);
        bool any = false;
        for (u32 e = 0; e < edge_count(); ++e) {
            if (!live_[edge_src_[e]] || !live_[edge_dst_[e]]) {
                drop_scratch_[e] = 1;
                any = true;
            }
        }
        if (any)
            drop_cargo(drop_scratch_.data());
    }
    ports_dirty_ = true;
    if (channels_)
        compute_routes();
}

// The graph among live sites (Phase 4 lifecycle): proximity, the fixed cut
// and long pairs (graph.h). Cargo on a link that survives keeps flying; cargo
// on a vanished one is lost. Link downtimes carry across the same way. Then
// neighbour numbering, ports and signal paths are recomputed.
void World::rebuild_graph() {
    Array<u64> old_keys;
    old_keys.resize(edge_src_.size());
    for (usize e = 0; e < edge_src_.size(); ++e)
        old_keys[e] = (static_cast<u64>(edge_src_[e]) << 32) | edge_dst_[e];
    Array<u64> old_down;
    old_down.assign(down_until_.data(), down_until_.size());

    assemble_graph(seed_, gp_, &pg_, live_.data());
    if (!build_edges(pg_.keys.data(), pg_.keys.size()))
        NOVA_FATAL("World::rebuild_graph: assembled graph failed validation");
    edge_latency_.assign(pg_.latency.data(), pg_.latency.size());
    edge_bandwidth_.assign(pg_.bandwidth.data(), pg_.bandwidth.size());
    edge_dist_.assign(pg_.dist.data(), pg_.dist.size());
    const usize ne = pg_.keys.size();
    auto find = [&](u64 key) -> u32 {
        usize lo = 0, hi = ne;
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (pg_.keys[mid] < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < ne && pg_.keys[lo] == key ? static_cast<u32>(lo) : kNone;
    };
    // Cargo, in send order, packed.
    Array<u8> codons;
    usize w = 0;
    for (usize i = 0; i < cargo_.size(); ++i) {
        Cargo c = cargo_[i];
        const u32 e = find(old_keys[c.edge]);
        if (e == kNone) {
            ++world_transit_lost_;
            continue;
        }
        c.edge = e;
        const u32 off = static_cast<u32>(codons.size());
        codons.append(cargo_codons_.data() + c.codon_off, c.len);
        c.codon_off = off;
        cargo_[w++] = c;
    }
    cargo_.resize(w);
    cargo_codons_ = static_cast<Array<u8>&&>(codons);
    down_until_.resize(ne);
    down_until_.fill(0);
    for (usize e = 0; e < old_keys.size(); ++e) {
        if (!old_down[e])
            continue;
        const u32 x = find(old_keys[e]);
        if (x != kNone)
            down_until_[x] = old_down[e];
    }
    init_ports();
}

// The devices (accel.gpu, accel.npu, accel.verify). Opened only for an async
// world; a device that won't start leaves its ops to the CPU reference, with a
// note. Nothing here changes a result.
namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Does `name` contain `part`, ignoring case?
bool name_has(const char* name, Str part) {
    const usize n = strlen(name);
    for (usize i = 0; i + part.n <= n; ++i) {
        usize k = 0;
        while (k < part.n && lower(name[i + k]) == lower(part.p[k]))
            ++k;
        if (k == part.n)
            return true;
    }
    return false;
}

} // namespace

void World::accel_open(Config& cfg) {
    gpu_.close();
    npu_.close();
    npu_on_gpu_ = false;
    acc_notes_.clear();
    acc_batches_ = acc_device_ops_ = acc_npu_ops_ = acc_reference_ops_ = acc_verified_ = acc_mismatches_ = 0;
    const Str g = cfg.get_str("accel.gpu", "auto");
    const Str npu = cfg.get_str("accel.npu", "auto");
    if (!npu.eq("auto") && !npu.eq("cpu") && !npu.eq("gpu"))
        cfg.fail("accel.npu", "must be auto, cpu or gpu");
    verify_every_ = get_u32_in(cfg, "accel.verify", 0, 0, 1000000000);
    if (!accel_async_ || (g.eq("cpu") && !npu.eq("auto")))
        return;
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 pick = kNone;
    for (u32 i = 0; i < n && pick == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware && (g.eq("auto") || name_has(devs[i].name, g)))
            pick = i;
    TextBuf why;
    if (g.eq("cpu")) {
        // GPU ops on the CPU reference, as asked.
    } else if (pick == kNone) {
        acc_notes_.append(g.eq("auto") ? "no hardware GPU found" : "no GPU matches accel.gpu");
        acc_notes_.append(": GPU ops run on the CPU reference\n");
    } else if (!gpu_.open(pick, &why)) {
        acc_notes_.append("GPU ");
        acc_notes_.append(devs[pick].name);
        acc_notes_.append(" failed to start (");
        acc_notes_.append(why.view());
        acc_notes_.append("): GPU ops run on the CPU reference\n");
    }
    // NPU ops: auto takes the NPU if it passes its self-test (exact answers
    // at every batch size), else the GPU; gpu sends them to the GPU kernel.
    if (npu.eq("auto")) {
        u32 at = kNone;
        for (u32 i = 0; i < n && at == kNone; ++i)
            if (devs[i].npu && devs[i].hardware)
                at = i;
        TextBuf nwhy;
        if (at != kNone && !npu_.open(at, &nwhy)) {
            acc_notes_.append("NPU ");
            acc_notes_.append(devs[at].name);
            acc_notes_.append(" failed its self-test (");
            acc_notes_.append(nwhy.view());
            acc_notes_.append(gpu_.is_open() ? "): NPU ops run on the GPU\n" : "): NPU ops run on the CPU reference\n");
        }
    }
    npu_on_gpu_ = gpu_.is_open() && !npu.eq("cpu");
}

const char* World::accel_gpu_name() const { return gpu_.is_open() ? gpu_.name() : "cpu"; }

// Phase 8: where warps run (warp.device: auto, gpu or cpu; warp.verify).
// auto and gpu take the first hardware GPU; one that won't start leaves warps
// to the CPU reference, with a note. Nothing here changes a result.
void World::warp_open(Config& cfg) {
    warp_gpu_.close();
    warp_notes_.clear();
    warp_gpu_rounds_ = warp_ref_rounds_ = warp_verified_ = warp_mismatches_ = 0;
    warp_gpu_nodes_ = warp_ref_nodes_ = 0;
    warp_compile_ms_ = 0;
    warp_failed_ = false;
    warp_limit_ = 1000000000u; // the search starts with every node on the GPU, and probes down
    warp_dir_ = 1;
    warp_stride_ = 1.0;
    warp_cost_ = 0;
    warp_cost_tick_ = 0;
    warp_auto_ticks_ = warp_plain_ticks_ = 0;
    warp_tick_s_[0] = warp_tick_s_[1] = 0;
    warp_last_plain_ = 2;
    const Str d = cfg.get_str("warp.device", "auto");
    if (!d.eq("auto") && !d.eq("gpu") && !d.eq("cpu") && !d.eq("mixed"))
        cfg.fail("warp.device", "must be auto, gpu, cpu or mixed");
    warp_auto_ = d.eq("auto");
    warp_mixed_ = d.eq("mixed"); // a test: nodes split between the two at random, every round
    warp_verify_ = get_u32_in(cfg, "warp.verify", 0, 0, 1000000000);
    if (!soup_params_.warps || d.eq("cpu"))
        return;
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 pick = kNone;
    for (u32 i = 0; i < n && pick == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            pick = i;
    TextBuf why;
    if (pick == kNone) {
        warp_notes_.append("no hardware GPU found: warps run on the CPU reference\n");
    } else if (!warp_gpu_.open(pick, soup_params_.warp_lanes, &why, &warp_compile_ms_)) {
        warp_notes_.append("GPU ");
        warp_notes_.append(devs[pick].name);
        warp_notes_.append(" failed to start warps (");
        warp_notes_.append(why.view());
        warp_notes_.append("): warps run on the CPU reference\n");
    }
}

// warp.device = auto, tick by tick: with the GPU (the split rounds of
// warp_step) or without it (every node's plain tick, as warp.device = cpu),
// whichever has taken less time lately. The first 10 ticks alternate; after
// that it tries the one not in use again, as the world changes: every 25th
// tick while the two are within a quarter of each other, every 100th while
// one is up to twice as quick, every 400th beyond. The choice never changes
// the world.
bool World::warp_go_plain() {
    const u64 k = warp_auto_ticks_++;
    if (k < 10)
        return (k & 1) != 0;
    const double a = warp_tick_s_[0], b = warp_tick_s_[1];
    const bool plain = b < a;
    const double ratio = plain ? (b > 0 ? a / b : 1e9) : (a > 0 ? b / a : 1e9);
    const u64 every = ratio < 1.25 ? 25 : (ratio < 2.0 ? 100 : 400);
    return k % every == 0 ? !plain : plain;
}

// The GPU failed while it held lanes: they are lost, so the world can't go
// on exactly. It stops here (resume from the last snapshot, on either device).
void World::warp_fail(Str why) {
    warp_notes_.append("GPU failed mid-run (");
    warp_notes_.append(why);
    warp_notes_.append("): the lanes it held are lost; the run stops here (resume from a snapshot)\n");
    warp_gpu_.close();
    warp_failed_ = true;
}

// Brings the listed warps' lanes back from the GPU; they are then the same
// on both sides.
bool World::warp_fetch(Array<WarpFetch>& list, Array<u32>& nodes) {
    if (list.empty())
        return true;
    TextBuf why;
    if (!warp_gpu_.fetch(list.data(), static_cast<u32>(list.size()), &why)) {
        warp_fail(why.view());
        return false;
    }
    for (usize i = 0; i < list.size(); ++i)
        soups_[nodes[i]].set_lane_state(list[i].slot, Soup::kLanesBoth);
    return true;
}

// Every warp of node n (or of every node) the GPU holds, back on the host:
// before hashing, saving, checking or showing them.
void World::warp_sync(u32 n) {
    if (!warp_gpu_.is_open() || model_ != Model::soup || !soup_params_.warps)
        return;
    Array<WarpFetch> list;
    Array<u32> nodes;
    const u32 first = n == kNone ? 0 : n, last = n == kNone ? nodes_ : n + 1;
    for (u32 k = first; k < last; ++k) {
        Soup& s = soups_[k];
        const u32 L = s.lanes_per_warp();
        for (usize w = 0; w < s.warp_slots(); ++w) {
            if (s.lane_state(static_cast<u32>(w)) != Soup::kLanesDevice)
                continue;
            list.push(WarpFetch{k, static_cast<u32>(w), s.lanes_mut().data() + w * L});
            nodes.push(k);
        }
    }
    warp_fetch(list, nodes);
}

void World::warp_first_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (!w->live_.empty() && !w->live_[node]) { // fallow (Phase 4): nothing runs
        w->soups_[node].idle(w->tick_);
        w->warp_active_[node] = 0;
        return;
    }
    if (w->channels_)
        w->signal_field(node);
    w->soups_[node].tick_first(w->tick_);
    w->warp_active_[node] = 1;
}

void World::warp_pack_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node])
        w->warp_active_[node] = w->soups_[node].warp_pack(w->warp_round_) ? 1 : 0;
}

void World::warp_ref_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node] && (!w->warp_on_gpu_[node] || w->warp_verifying_))
        w->soups_[node].warp_run_ref();
}

void World::warp_unpack_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node])
        w->soups_[node].warp_unpack(w->warp_on_gpu_[node] && !w->warp_verifying_);
}

void World::warp_host_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node])
        w->soups_[node].warp_host(w->warp_round_);
}

void World::warp_second_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->live_.empty() || w->live_[node])
        w->soups_[node].tick_second();
}

// The update with warps on the GPU: every node's first half (sparks, CPUs),
// then each round -- every node packs; the GPU runs its share of the nodes
// while the CPU threads run the rest on the reference; every node unpacks
// and does its host instructions -- then every node's second half. Exactly
// what Soup::tick does node by node on the reference: whichever device runs
// a node's round, the world is the same.
//
// The GPU keeps each warp's lanes between rounds; the soup knows which side
// holds them (Soup::lane_state). Lanes the host changed go up with the round
// that runs them; lanes the host needs (a host instruction, a node moving to
// the reference, a hash) come back first.
//
// The split (warp.device = auto): a GPU runs a body's steps one after
// another, so a node whose longest body ran more than warp_limit_ steps last
// round goes to the CPU; the limit is searched for, round by round, as the
// one that makes a round quickest per warp step (below). warp.device = gpu
// sends every node to the GPU.
void World::warp_step(ThreadPool& pool) {
    double cpu_s = 0, gpu_s = 0, pack_s = 0;
    warp_active_.resize(nodes_);
    warp_on_gpu_.resize(nodes_);
    pool.run(nodes_, &World::warp_first_task, this);
    for (u32 r = 0; r < soup_params_.warp_rounds && !warp_failed_; ++r) {
        warp_round_ = r;
        double t = clock_seconds();
        pool.run(nodes_, &World::warp_pack_task, this);
        // The constants must be the same everywhere for one dispatch.
        const Array<u32>* cst = nullptr;
        bool same = true;
        for (u32 n = 0; n < nodes_; ++n) {
            if (!warp_active_[n])
                continue;
            const Array<u32>& c = soups_[n].warp_consts();
            if (!cst)
                cst = &c;
            else
                same = same && c.size() == cst->size() && memcmp(c.data(), cst->data(), c.size() * 4) == 0;
        }
        if (!cst)
            break; // no node has warps to run
        // Which nodes the GPU takes.
        u32 gpu_nodes = 0, ref_nodes = 0, longest_all = 0;
        for (u32 n = 0; n < nodes_; ++n) {
            warp_on_gpu_[n] = 0;
            if (!warp_active_[n])
                continue;
            const u32 chain = soups_[n].warp_longest();
            longest_all = chain > longest_all ? chain : longest_all;
            bool gpu = same && (!warp_auto_ || chain <= warp_limit_);
            if (warp_mixed_) // the test mode: lanes cross between the two all the time
                gpu = same && (mix64((tick_ << 20) ^ (static_cast<u64>(r) << 16) ^ n ^ seed_) & 1);
            warp_on_gpu_[n] = gpu ? 1 : 0;
            if (gpu)
                ++gpu_nodes;
            else
                ++ref_nodes;
        }
        // warp.verify: this round on both, compared (the GPU's nodes' inputs
        // must then be on the host too).
        warp_verifying_ = warp_verify_ && gpu_nodes && (warp_gpu_rounds_ % warp_verify_ == 0);
        // Lanes the reference needs that only the GPU has.
        Array<WarpFetch>& fl = warp_fetch_;
        Array<u32>& fn = warp_fetch_node_;
        fl.clear();
        fn.clear();
        for (u32 n = 0; n < nodes_; ++n) {
            if (!warp_active_[n] || (warp_on_gpu_[n] && !warp_verifying_))
                continue;
            Soup& s = soups_[n];
            const u32 L = s.lanes_per_warp();
            const u32* I = s.warp_image().data();
            for (usize k = 0; k < s.warp_records().size(); ++k) {
                const u32 rec = s.warp_records()[k];
                for (u32 i = 0; i < I[rec + WB_NWARP]; ++i) {
                    const u32 w = I[rec + WB_WARPS + i * WW_SIZE + WW_LANE0] / L;
                    if (s.lane_state(w) == Soup::kLanesDevice) {
                        fl.push(WarpFetch{n, w, s.lanes_mut().data() + static_cast<usize>(w) * L});
                        fn.push(n);
                    }
                }
            }
        }
        if (!warp_fetch(fl, fn))
            return;
        // The GPU's jobs: each node's records, and the lanes it must be sent
        // (those of the round's warps the host holds the newer copy of).
        warp_jobs_.clear();
        warp_job_node_.clear();
        warp_send_.clear();
        Array<u32> send_off;
        for (u32 n = 0; n < nodes_; ++n) {
            if (!warp_active_[n] || !warp_on_gpu_[n])
                continue;
            Soup& s = soups_[n];
            const u32 L = s.lanes_per_warp();
            const u32* I = s.warp_image().data();
            send_off.push(static_cast<u32>(warp_send_.size()));
            for (usize k = 0; k < s.warp_records().size(); ++k) {
                const u32 rec = s.warp_records()[k];
                for (u32 i = 0; i < I[rec + WB_NWARP]; ++i) {
                    const u32 w = I[rec + WB_WARPS + i * WW_SIZE + WW_LANE0] / L;
                    if (s.lane_state(w) == Soup::kLanesHost)
                        warp_send_.push(w);
                }
            }
            WarpJob j = {};
            j.node = n;
            j.img = s.warp_image().data();
            j.img_words = static_cast<u32>(s.warp_image().size());
            j.recs = s.warp_records().data();
            j.nrec = static_cast<u32>(s.warp_records().size());
            j.lanes = s.lanes_mut().data();
            j.nlanes = static_cast<u32>(s.lanes_mut().size());
            warp_jobs_.push(j);
            warp_job_node_.push(n);
        }
        for (usize i = 0; i < warp_jobs_.size(); ++i) { // (the send lists, now that they've stopped growing)
            const u32 a = send_off[i], b = i + 1 < warp_jobs_.size() ? send_off[i + 1] : static_cast<u32>(warp_send_.size());
            warp_jobs_[i].send = warp_send_.data() + a;
            warp_jobs_[i].nsend = b - a;
        }
        // warp.verify: keep the GPU's nodes' inputs, to run the reference on them.
        if (warp_verifying_) {
            warp_save_img_.clear();
            warp_save_lanes_.clear();
            warp_save_off_.clear();
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                warp_save_off_.push(warp_save_img_.size());
                warp_save_off_.push(warp_save_lanes_.size());
                warp_save_img_.append(warp_jobs_[i].img, warp_jobs_[i].img_words);
                warp_save_lanes_.append(static_cast<const u32*>(warp_jobs_[i].lanes),
                                        static_cast<usize>(warp_jobs_[i].nlanes) * (sizeof(Lane) / 4));
            }
        }
        pack_s += clock_seconds() - t;
        cpu_s += clock_seconds() - t;
        // The GPU starts on its nodes; the CPU threads run the rest meanwhile.
        t = clock_seconds();
        TextBuf why;
        if (!warp_jobs_.empty() &&
            !warp_gpu_.submit(cst->data(), static_cast<u32>(cst->size()), warp_jobs_.data(),
                              static_cast<u32>(warp_jobs_.size()), &why, &pool)) {
            warp_fail(why.view());
            return;
        }
        if (ref_nodes && !warp_verifying_)
            pool.run(nodes_, &World::warp_ref_task, this);
        if (!warp_jobs_.empty() && !warp_gpu_.finish(&why, &pool)) {
            warp_fail(why.view());
            return;
        }
        const double round_s = clock_seconds() - t; // the GPU and the CPU at once
        gpu_s += round_s;
        t = clock_seconds();
        if (!warp_jobs_.empty())
            ++warp_gpu_rounds_;
        if (ref_nodes)
            ++warp_ref_rounds_;
        warp_gpu_nodes_ += gpu_nodes;
        warp_ref_nodes_ += ref_nodes;
        if (warp_verifying_) {
            // The GPU's result aside (its images, and its lanes fetched), the
            // inputs back, the reference on them, and the two compared. The
            // reference's result stays, so those lanes are the host's.
            Array<u32> gpu_img, gpu_lanes;
            Array<WarpFetch> all;
            Array<u32> all_node;
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                const WarpJob& j = warp_jobs_[i];
                gpu_img.append(j.img, j.img_words);
                memcpy(j.img, warp_save_img_.data() + warp_save_off_[2 * i], static_cast<usize>(j.img_words) * 4);
            }
            gpu_lanes.resize(warp_save_lanes_.size());
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                const WarpJob& j = warp_jobs_[i];
                const u32 n = warp_job_node_[i];
                const u32 L = soups_[n].lanes_per_warp();
                u32* base = gpu_lanes.data() + warp_save_off_[2 * i + 1];
                const u32* I = j.img;
                for (u32 k = 0; k < j.nrec; ++k)
                    for (u32 q = 0; q < I[j.recs[k] + WB_NWARP]; ++q) {
                        const u32 w = I[j.recs[k] + WB_WARPS + q * WW_SIZE + WW_LANE0] / L;
                        all.push(WarpFetch{n, w, base + static_cast<usize>(w) * L * (sizeof(Lane) / 4)});
                        all_node.push(n);
                    }
            }
            if (!all.empty() && !warp_gpu_.fetch(all.data(), static_cast<u32>(all.size()), &why)) {
                warp_fail(why.view());
                return;
            }
            pool.run(nodes_, &World::warp_ref_task, this);
            usize io = 0;
            bool equal = true;
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                const WarpJob& j = warp_jobs_[i];
                equal = equal && memcmp(gpu_img.data() + io, j.img, static_cast<usize>(j.img_words) * 4) == 0;
                io += j.img_words;
            }
            for (usize i = 0; i < all.size(); ++i) { // the round's lanes, the GPU's against the reference's
                const u32 L = soups_[all_node[i]].lanes_per_warp();
                const Lane* ref = soups_[all_node[i]].lanes_mut().data() + static_cast<usize>(all[i].slot) * L;
                equal = equal && memcmp(all[i].dst, ref, L * sizeof(Lane)) == 0;
            }
            ++warp_verified_;
            if (!equal) {
                if (warp_mismatches_ == 0) {
                    warp_notes_.append("warp.verify: the GPU's round differed from the reference at tick ");
                    warp_notes_.append_u64(tick_);
                    warp_notes_.append(" (the reference's result was kept)\n");
                }
                ++warp_mismatches_;
            }
        }
        u64 steps_before = 0, steps_after = 0; // this round's warp steps, on both
        if (warp_auto_)
            for (u32 n = 0; n < nodes_; ++n)
                steps_before += soups_[n].tally().warp_steps;
        pool.run(nodes_, &World::warp_unpack_task, this);
        if (warp_auto_)
            for (u32 n = 0; n < nodes_; ++n)
                steps_after += soups_[n].tally().warp_steps;
        // The lanes host instructions need: the GPU's warps that wait on one.
        fl.clear();
        fn.clear();
        if (!warp_verifying_) {
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                const WarpJob& j = warp_jobs_[i];
                const u32 n = warp_job_node_[i];
                Soup& s = soups_[n];
                const u32 L = s.lanes_per_warp();
                for (u32 k = 0; k < j.nrec; ++k)
                    for (u32 q = 0; q < j.img[j.recs[k] + WB_NWARP]; ++q) {
                        const u32 e = j.recs[k] + WB_WARPS + q * WW_SIZE;
                        if (!j.img[e + WW_WAIT])
                            continue;
                        const u32 w = j.img[e + WW_LANE0] / L;
                        fl.push(WarpFetch{n, w, s.lanes_mut().data() + static_cast<usize>(w) * L});
                        fn.push(n);
                    }
            }
        }
        if (!warp_fetch(fl, fn))
            return;
        pool.run(nodes_, &World::warp_host_task, this);
        cpu_s += clock_seconds() - t;
        // The split (auto): a search for the limit that makes rounds quickest,
        // measured. The cost of a round is its wall time, the GPU and the CPU
        // working at once, per warp step it ran (so a world that grows doesn't
        // read as a split that got worse). Each round moves the limit by a
        // factor of 2^stride the way that helped last time; a round that got
        // dearer turns back and halves the stride, one that got cheaper keeps
        // on a little faster. Past twice the longest chain every node is the
        // GPU's, so the search can settle there (as in GPU-heavy worlds) and
        // still probe back down; near 0 nearly every node is the CPU's.
        if (warp_auto_ && !warp_verifying_ && steps_after > steps_before) {
            const double cost = round_s / static_cast<double>(steps_after - steps_before);
            // Rounds are compared only tick to tick: after ticks without the
            // GPU the world has moved on, so the first round just measures.
            const bool follows = warp_cost_ > 0 && warp_cost_tick_ + 1 >= tick_;
            if (follows && cost > warp_cost_ * 1.02) {
                warp_dir_ = -warp_dir_;
                warp_stride_ = warp_stride_ * 0.5 < 0.125 ? 0.125 : warp_stride_ * 0.5;
            } else if (follows && cost < warp_cost_) {
                warp_stride_ = warp_stride_ * 1.25 > 2.0 ? 2.0 : warp_stride_ * 1.25;
            }
            warp_cost_ = cost;
            warp_cost_tick_ = tick_;
            const double top = 2.0 * (static_cast<double>(longest_all) + 1.0);
            double lim = (warp_limit_ < 1 ? 1.0 : static_cast<double>(warp_limit_)) * pow(2.0, warp_dir_ * warp_stride_);
            lim = lim > top ? top : lim;
            warp_limit_ = lim > 1e9 ? 1000000000u : static_cast<u32>(lim);
        }
    }
    pool.run(nodes_, &World::warp_second_task, this);
    if (prof_) {
        prof_->warp_cpu += cpu_s;
        prof_->warp_pack += pack_s;
        prof_->warp_host += cpu_s - pack_s;
        prof_->warp_gpu += gpu_s;
        prof_->update -= cpu_s + gpu_s; // step() adds the whole update; these are their own
    }
}

const char* World::accel_npu_name() const {
    if (npu_.is_open())
        return npu_.name();
    return gpu_.is_open() && npu_on_gpu_ ? gpu_.name() : "cpu";
}

// The barrier's asynchronous accelerator ops (Phase 5, design.md 6.11): every
// node's ops issued this tick join the batch in flight, in node order then
// issue order, with their inputs read now; then every op due at this barrier
// is handed back to its node, in the same order. The new batch goes to the
// devices before the due ops are applied, so with latency 2 or more it runs
// while the CPUs work on the next tick. Phase 9c: each device has its own
// latency (with a timed table, its measured round trip), so the ops due now
// are those whose due tick has come, taken in the order they were issued; and
// every batch comes home at the barrier after it was sent, its results held
// until due, so a long latency holds no device slot.
void World::accel_barrier() {
    const usize first = acc_ops_.size();
    for (u32 n = 0; n < nodes_; ++n)
        if (live(n))
            soups_[n].accel_gather(&acc_ops_, &acc_in_, &acc_addr_);
    while (acc_job_.size() < acc_ops_.size()) {
        acc_job_.push(-1);
        acc_slot_.push(0);
        acc_held_scalar_.push(0);
        acc_held_flags_.push(0);
    }
    accel_bring_home(first);
    bool any_done = accel_complete_range(0, first);
    if ((gpu_.is_open() || npu_.is_open()) && acc_ops_.size() > first)
        accel_submit(first);
    any_done = accel_complete_range(first, acc_ops_.size()) || any_done;
    if (!any_done)
        return;
    // Keep the rest, in order, with their inputs, addresses and held results packed.
    Array<u8> in, held;
    Array<u32> addr;
    usize w = 0;
    for (usize i = 0; i < acc_ops_.size(); ++i) {
        AccelOp op = acc_ops_[i];
        if (op.due <= tick_)
            continue;
        const u32 reads = op.src_n + op.dst_n + op.mat_n;
        const u32 in_off = static_cast<u32>(in.size()), addr_off = static_cast<u32>(addr.size());
        in.append(acc_in_.data() + op.in_off, reads);
        addr.append(acc_addr_.data() + op.addr_off, op.out_n);
        op.in_off = in_off;
        op.addr_off = addr_off;
        acc_job_[w] = acc_job_[i];
        acc_slot_[w] = acc_slot_[i];
        if (acc_job_[i] == -2) { // its held result moves too
            acc_slot_[w] = static_cast<u32>(held.size());
            held.append(acc_held_.data() + acc_slot_[i], op.out_n);
        }
        acc_held_scalar_[w] = acc_held_scalar_[i];
        acc_held_flags_[w] = acc_held_flags_[i];
        acc_ops_[w++] = op;
    }
    acc_ops_.resize(w);
    acc_job_.resize(w);
    acc_slot_.resize(w);
    acc_held_scalar_.resize(w);
    acc_held_flags_.resize(w);
    acc_in_ = static_cast<Array<u8>&&>(in);
    acc_addr_ = static_cast<Array<u32>&&>(addr);
    acc_held_ = static_cast<Array<u8>&&>(held);
}

// Phase 9c: a batch sent at an earlier barrier has had a tick: it comes home
// now and its results wait with their ops until due, freeing its device slot.
void World::accel_bring_home(usize first) {
    for (usize i = 0; i < first; ++i) {
        const i32 t = acc_job_[i];
        if (t < 0)
            continue;
        const bool ok = accel_collect(t);
        const AccelJob& j = acc_jobs_[t];
        if (!ok) {
            acc_job_[i] = -1; // the device failed: the reference, at its due tick
            continue;
        }
        const AccelOp& op = acc_ops_[i];
        const u32 off = static_cast<u32>(acc_held_.size());
        acc_held_.append(j.codons.data() + j.out_off[acc_slot_[i]], op.out_n);
        acc_held_scalar_[i] = j.scalars[acc_slot_[i]];
        acc_held_flags_[i] = static_cast<u8>((j.verify ? 1 : 0) | (static_cast<u32>(t) >= kAccelSlots ? 2 : 0));
        acc_slot_[i] = off;
        acc_job_[i] = -2;
    }
}

// Waits for batch `job` the first time it is asked for, closing its device
// if it failed (the reference takes its ops from then on).
bool World::accel_collect(i32 t) {
    AccelJob& j = acc_jobs_[t];
    if (j.ready)
        return j.ok;
    j.ready = true;
    const bool on_npu = static_cast<u32>(t) >= kAccelSlots;
    j.ok = on_npu ? npu_.collect(t - static_cast<i32>(kAccelSlots), j.codons.data(), j.scalars.data())
                  : gpu_.collect(t, j.codons.data(), j.scalars.data());
    if (!j.ok && (on_npu ? npu_.is_open() : gpu_.is_open())) {
        acc_notes_.append(on_npu ? "NPU " : "GPU ");
        acc_notes_.append(on_npu ? npu_.name() : gpu_.name());
        acc_notes_.append(" failed at tick ");
        acc_notes_.append_u64(tick_);
        acc_notes_.append(on_npu ? (npu_on_gpu_ ? ": NPU ops run on the GPU from here\n"
                                                : ": NPU ops run on the CPU reference from here\n")
                                 : ": GPU ops run on the CPU reference from here\n");
        if (on_npu)
            npu_.close();
        else
            gpu_.close();
    }
    return j.ok;
}

// Sends the ops gathered from `first` on to the devices, one batch each: the
// NPU's kinds to the NPU; the GPU's kinds, and the NPU's when there is no NPU
// (or it refused the batch) and accel.npu allows it, to the GPU. No free slot
// or a device error leaves them to the reference.
void World::accel_submit(usize first) {
    bool npu_took = false;
    if (npu_.is_open()) {
        acc_pick_.clear();
        for (usize i = first; i < acc_ops_.size(); ++i)
            if (accel_is_npu(acc_ops_[i].kind))
                acc_pick_.push(static_cast<u32>(i));
        if (acc_pick_.size()) {
            const int t =
                npu_.submit(acc_ops_.data(), acc_pick_.data(), static_cast<u32>(acc_pick_.size()), acc_in_.data());
            if (t >= 0) {
                accel_take_job(kAccelSlots + static_cast<u32>(t));
                npu_took = true;
            }
        }
    }
    if (gpu_.is_open()) {
        acc_pick_.clear();
        for (usize i = first; i < acc_ops_.size(); ++i)
            if (!accel_is_npu(acc_ops_[i].kind) || (npu_on_gpu_ && !npu_took))
                acc_pick_.push(static_cast<u32>(i));
        if (acc_pick_.size()) {
            const int t =
                gpu_.submit(acc_ops_.data(), acc_pick_.data(), static_cast<u32>(acc_pick_.size()), acc_in_.data());
            if (t >= 0)
                accel_take_job(static_cast<u32>(t));
        }
    }
}

void World::accel_take_job(u32 job) {
    const u32 n = static_cast<u32>(acc_pick_.size());
    AccelJob& j = acc_jobs_[job];
    j.ready = j.ok = false;
    j.verify = verify_every_ && acc_batches_ % verify_every_ == 0;
    ++acc_batches_;
    j.out_off.resize(n);
    j.scalars.resize(n);
    u32 off = 0;
    for (u32 k = 0; k < n; ++k) {
        const u32 i = acc_pick_[k];
        acc_job_[i] = static_cast<i32>(job);
        acc_slot_[i] = k;
        j.out_off[k] = off;
        off += acc_ops_[i].out_n;
    }
    j.codons.resize(off ? off : 1);
}

bool World::accel_complete_range(usize lo, usize hi) {
    bool any = false;
    for (usize i = lo; i < hi; ++i) {
        const AccelOp& op = acc_ops_[i];
        if (op.due > tick_)
            continue;
        any = true;
        const u8* in = acc_in_.data() + op.in_off;
        const u8* out = nullptr;
        u32 scalar = 0;
        const i32 t = acc_job_[i];
        bool device = false, verify = false, on_npu = false;
        static const u8 kNoCodons = 0; // an op with a scalar result only has no codons to point at
        if (t == -2) { // brought home earlier, held until now (9c)
            device = true;
            out = op.out_n ? acc_held_.data() + acc_slot_[i] : &kNoCodons;
            scalar = acc_held_scalar_[i];
            verify = (acc_held_flags_[i] & 1) != 0;
            on_npu = (acc_held_flags_[i] & 2) != 0;
        } else if (t >= 0 && accel_collect(t)) { // the first of its batch waits for the device
            const AccelJob& j = acc_jobs_[t];
            device = true;
            out = j.codons.data() + j.out_off[acc_slot_[i]];
            scalar = j.scalars[acc_slot_[i]];
            verify = j.verify;
            on_npu = static_cast<u32>(t) >= kAccelSlots;
        }
        if (device) {
            ++acc_device_ops_;
            acc_npu_ops_ += on_npu ? 1 : 0;
            if (verify) {
                acc_out_.resize(op.out_n);
                const u32 want = accel_reference(op, in, acc_out_.data());
                ++acc_verified_;
                const bool same = (!accel_has_scalar(op.kind) || want == scalar) &&
                                  (op.out_n == 0 || memcmp(acc_out_.data(), out, op.out_n) == 0);
                if (!same) {
                    if (acc_mismatches_ == 0) {
                        char b[200];
                        snprintf(b, sizeof b,
                                 "verify: tick %llu node %u op kind %u n %u differs from the reference "
                                 "(result %u, reference %u)\n",
                                 static_cast<unsigned long long>(tick_), op.node, op.kind, op.n, scalar, want);
                        acc_notes_.append(b);
                    }
                    ++acc_mismatches_;
                    out = acc_out_.data();
                    scalar = want;
                }
            }
        } else {
            ++acc_reference_ops_;
            acc_out_.resize(op.out_n);
            scalar = accel_reference(op, in, acc_out_.data());
            out = acc_out_.data();
        }
        soups_[op.node].accel_complete(op, out, scalar, acc_addr_.data() + op.addr_off);
    }
    return any;
}

u64 World::links_down() const {
    u64 n = 0;
    for (usize e = 0; e < link_down_.size(); ++e)
        n += link_down_[e];
    return n / 2;
}

u64 World::live_nodes() const {
    if (live_.empty())
        return nodes_;
    u64 n = 0;
    for (usize i = 0; i < live_.size(); ++i)
        n += live_[i];
    return n;
}

// Transit corruption (Phase 4): each codon flips one random bit at
// edge.corruption per tick in transit, sampled as a Poisson process over the
// cargo's exposure (codons x ticks) from a stream keyed by its send number, so
// the result doesn't depend on delivery order.
void World::corrupt(Cargo& c) {
    const double exposure = static_cast<double>(c.len) * c.ticks;
    Rng r(seed_, Rng::kWorldNode, 0x636F727275707400ull + c.seq); // "corrupt"
    double pos = -log(1.0 - r.unit()) / edge_corruption_;
    while (pos < exposure) {
        u32 i = static_cast<u32>(pos / c.ticks);
        if (i >= c.len)
            i = c.len - 1;
        cargo_codons_[c.codon_off + i] ^= static_cast<u8>(1u << r.below(6));
        ++world_corrupted_;
        pos += -log(1.0 - r.unit()) / edge_corruption_;
    }
}

void World::drop_cargo(const u8* drop) {
    Array<u8> codons;
    usize w = 0;
    for (usize i = 0; i < cargo_.size(); ++i) {
        Cargo c = cargo_[i];
        if (drop[c.edge]) {
            --in_flight_[c.edge];
            ++world_transit_lost_;
            continue;
        }
        const u32 off = static_cast<u32>(codons.size());
        codons.append(cargo_codons_.data() + c.codon_off, c.len);
        c.codon_off = off;
        cargo_[w++] = c;
    }
    cargo_.resize(w);
    cargo_codons_ = static_cast<Array<u8>&&>(codons);
}

// Link failures (Phase 4), at the barrier after delivery: links whose downtime
// is over come back for the next tick; then each link that is up fails with
// chance edge.failure, both directions together, for an exponential downtime
// of mean edge.downtime ticks (at least 1), losing whatever it carries.
void World::links_update() {
    const u32 e = edge_count();
    bool changed = false;
    for (u32 i = 0; i < e; ++i) {
        if (down_until_[i] && down_until_[i] <= tick_ + 1) {
            down_until_[i] = 0;
            link_down_[i] = 0;
            changed = true;
        }
    }
    Rng r(seed_, Rng::kWorldNode, 0x6661696C00000000ull + tick_); // "fail"
    drop_scratch_.resize(e);
    drop_scratch_.fill(0);
    bool any = false;
    for (u32 i = 0; i < e; ++i) {
        if (edge_src_[i] > edge_dst_[i] || down_until_[i] || !r.chance(edge_failure_))
            continue;
        const u64 d = 1 + static_cast<u64>(floor(-log(1.0 - r.unit()) * edge_downtime_));
        const u32 j = edge_rev_[i];
        down_until_[i] = down_until_[j] = tick_ + 1 + d;
        link_down_[i] = link_down_[j] = 1;
        drop_scratch_[i] = drop_scratch_[j] = 1;
        any = true;
    }
    if (any)
        drop_cargo(drop_scratch_.data());
    if (any || changed)
        ports_dirty_ = true; // a landscape's ports read the combined flags
    if ((any || changed) && channels_) // a down link carries no signal
        compute_routes();
}

void World::apply_spark_rate(double rate) {
    soup_params_.spark_rate = rate;
    for (u32 n = 0; n < node_params_.size(); ++n)
        node_params_[n].spark_rate = rate * spark_factor_[n];
}

// The barrier, part 1: every node's departures this tick, in node order and
// then the order they left, go onto their edges.
void World::take_departures() {
    for (u32 n = 0; n < nodes_; ++n) {
        Soup& s = soups_[n];
        const Array<Departure>& d = s.departures();
        for (usize i = 0; i < d.size(); ++i) {
            const Departure& dep = d[i];
            const u32 e = port_list_[out_begin_[n] + dep.nbr];
            const u32 bw = edge_bandwidth_[e];
            Cargo c = {};
            c.due = tick_ + (dep.len + bw - 1) / bw + edge_latency_[e];
            c.seq = next_seq_++;
            c.tag = dep.tag;
            c.edge = e;
            c.len = dep.len;
            c.codon_off = static_cast<u32>(cargo_codons_.size());
            c.millicores = dep.millicores;
            c.gpus = dep.gpus;
            c.npus = dep.npus;
            c.disk = dep.disk;
            c.form = dep.form;
            c.emillicores = dep.emillicores;
            c.kind = dep.kind;
            c.ticks = static_cast<u32>(c.due - tick_);
            cargo_codons_.append(s.departure_codons() + dep.codon_off, dep.len);
            cargo_.push(c);
            ++in_flight_[e];
        }
        s.clear_departures();
    }
}

// The barrier, part 2: every cargo due at this tick arrives, in the fixed
// order (destination id, source id, send order; design.md 7.1 D).
void World::deliver_cargo() {
    due_scratch_.clear();
    for (usize i = 0; i < cargo_.size(); ++i)
        if (cargo_[i].due <= tick_)
            due_scratch_.push(static_cast<u32>(i));
    if (due_scratch_.empty())
        return;
    NOVA_ASSERT(due_scratch_.size() < (1u << 24) && nodes_ <= (1u << 20));
    order_scratch_.clear();
    for (usize j = 0; j < due_scratch_.size(); ++j) { // cargo_ is in send order
        const u32 e = cargo_[due_scratch_[j]].edge;
        order_scratch_.push((static_cast<u64>(edge_dst_[e]) << 44) | (static_cast<u64>(edge_src_[e]) << 24) | j);
    }
    sort_u64(order_scratch_.data(), order_scratch_.size());
    for (usize i = 0; i < order_scratch_.size(); ++i) {
        Cargo& c = cargo_[due_scratch_[order_scratch_[i] & 0xFFFFFF]];
        if (edge_corruption_ > 0)
            corrupt(c);
        soups_[edge_dst_[c.edge]].arrive(cargo_codons_.data() + c.codon_off, c.len, c.millicores, c.gpus,
                                         c.npus, c.disk, c.tag, c.form, c.emillicores, static_cast<u8>(c.kind));
        --in_flight_[c.edge];
    }
    // Keep the rest, in order, with their codons packed.
    Array<u8> codons;
    usize w = 0;
    for (usize i = 0; i < cargo_.size(); ++i) {
        if (cargo_[i].due <= tick_)
            continue;
        Cargo c = cargo_[i];
        const u32 off = static_cast<u32>(codons.size());
        codons.append(cargo_codons_.data() + c.codon_off, c.len);
        c.codon_off = off;
        cargo_[w++] = c;
    }
    cargo_.resize(w);
    cargo_codons_ = static_cast<Array<u8>&&>(codons);
}

KnobResult World::check_knob(Str key, Str value) const {
    KnobValue kv;
    return parse_knob(key, value, model_, soup_size_, dispersal_, &kv);
}

const char* const kSoupKnobs[] = {"spark.rate", "spark.grant", "spark.min_len", "spark.max_len",
                                  "cpu.wear",   "heat.ambient", "edge.corruption"};

u32 World::knob_count() const {
    if (model_ != Model::soup)
        return 0;
    return dispersal_ ? 7 : 6;
}

Str World::knob_key(u32 i) const { return i < knob_count() ? Str(kSoupKnobs[i]) : Str(); }

double World::knob_now(u32 i) const {
    const SoupParams& p = soup_params_;
    switch (i) {
    case 0:
        return p.spark_rate;
    case 1:
        return p.spark_grant;
    case 2:
        return p.spark_min;
    case 3:
        return p.spark_max;
    case 4:
        return p.cpu_wear;
    case 5:
        return p.ambient;
    case 6:
        return edge_corruption_;
    default:
        return 0;
    }
}

KnobResult World::set_knob(Str key, Str value) {
    KnobValue kv;
    KnobResult r = parse_knob(key, value, model_, soup_size_, dispersal_, &kv);
    if (r != KnobResult::ok)
        return r;
    switch (kv.knob) {
    // Phase 6: one number for every node (none of them is varied per node).
    case Knob::cpu_wear:
        soup_params_.cpu_wear = kv.f;
        for (u32 n = 0; n < node_params_.size(); ++n)
            node_params_[n].cpu_wear = kv.f;
        break;
    case Knob::heat_ambient:
        soup_params_.ambient = static_cast<float>(kv.f);
        for (u32 n = 0; n < node_params_.size(); ++n)
            node_params_[n].ambient = static_cast<float>(kv.f);
        break;
    case Knob::edge_corruption:
        edge_corruption_ = kv.f;
        break;
    case Knob::send_rate:
        send_rate_ = kv.f;
        break;
    case Knob::sends_per_tick:
        sends_per_tick_ = kv.u;
        break;
    case Knob::mutations_per_tick:
        mutations_per_tick_ = kv.u;
        break;
    case Knob::max_parcel:
        max_parcel_ = kv.u;
        break;
    // Spark knobs reach every node: the rate times the node's factor (Phase 3),
    // the lengths clamped to the node's size.
    case Knob::spark_rate:
        apply_spark_rate(kv.f);
        break;
    case Knob::spark_grant:
        soup_params_.spark_grant = kv.u;
        for (u32 n = 0; n < node_params_.size(); ++n)
            node_params_[n].spark_grant = kv.u;
        break;
    case Knob::spark_min:
        soup_params_.spark_min = kv.u;
        for (u32 n = 0; n < node_params_.size(); ++n)
            node_params_[n].spark_min = min_of(kv.u, node_params_[n].size);
        break;
    case Knob::spark_max:
        soup_params_.spark_max = kv.u;
        for (u32 n = 0; n < node_params_.size(); ++n)
            node_params_[n].spark_max = min_of(kv.u, node_params_[n].size);
        break;
    case Knob::none:
        break;
    }
    return KnobResult::ok;
}

void World::update_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->model_ == Model::soup) {
        if (!w->live_.empty() && !w->live_[node]) { // fallow (Phase 4): nothing runs
            w->soups_[node].idle(w->tick_);
            return;
        }
        if (w->channels_) // Phase 4: what this node hears this tick
            w->signal_field(node);
        w->soups_[node].tick(w->tick_);
    } else {
        w->update_node(node);
    }
}

void World::update_node(u32 n) {
    Rng r(seed_, n, tick_);
    u8* s = soup_.data() + static_cast<usize>(n) * soup_size_;
    Tally t = {};

    for (u32 m = 0; m < mutations_per_tick_; ++m) {
        u32 pos = static_cast<u32>(r.below(soup_size_));
        s[pos] = static_cast<u8>(r.next() & 63);
        ++t.mutations;
    }

    u32 first = out_begin_[n];
    u32 degree = out_begin_[n + 1] - first;
    for (u32 k = 0; k < sends_per_tick_ && degree > 0; ++k) {
        if (!r.chance(send_rate_))
            continue;
        u32 have = pool_[n];
        if (have == 0)
            continue;
        u32 e = first + static_cast<u32>(r.below(degree));
        u32 amount = 1 + static_cast<u32>(r.below(min_of(have, max_parcel_)));
        u8 codon = s[r.below(soup_size_)];
        u32& count = outbox_count_[e];
        if (count >= outbox_capacity_) {
            ++t.refused;
            continue;
        }
        Parcel& p = outbox_[static_cast<usize>(e) * outbox_capacity_ + count];
        p.amount = amount;
        p.codon = codon;
        p.pad[0] = p.pad[1] = p.pad[2] = 0;
        ++count;
        pool_[n] = have - amount;
        ++t.sent;
    }

    tally_[n] = t;
    rng_counter_[n] = r.counter();
}

void World::deliver() {
    TickStats st = {};
    for (u32 n = 0; n < nodes_; ++n) {
        st.parcels_sent += tally_[n].sent;
        st.parcels_refused += tally_[n].refused;
        st.mutations += tally_[n].mutations;
    }

    for (u32 v = 0; v < nodes_; ++v) {
        // Continue v's own stream where its update phase left it.
        Rng r(seed_, v, tick_);
        r.set_counter(rng_counter_[v]);
        u8* s = soup_.data() + static_cast<usize>(v) * soup_size_;
        for (u32 k = in_begin_[v]; k < in_begin_[v + 1]; ++k) {
            u32 e = in_list_[k];
            u32 src = edge_src_[e];
            const Parcel* box = outbox_.data() + static_cast<usize>(e) * outbox_capacity_;
            for (u32 j = 0; j < outbox_count_[e]; ++j) {
                u32 room = pool_[v] >= pool_capacity_ ? 0 : pool_capacity_ - pool_[v];
                u32 accepted = min_of(box[j].amount, room);
                u32 bounced = box[j].amount - accepted;
                pool_[v] += accepted;
                pool_[src] += bounced;
                if (accepted > 0)
                    s[r.below(soup_size_)] = box[j].codon;
                ++st.parcels_delivered;
                st.units_accepted += accepted;
                st.units_bounced += bounced;
            }
            outbox_count_[e] = 0;
        }
    }
    last_ = st;
}

void World::step(ThreadPool& pool) {
    // Profiling (Phase 7): each part's wall time, only when asked for.
    double t = prof_ ? clock_seconds() : 0;
    auto lap = [&](double* slot) {
        if (!prof_)
            return;
        const double now = clock_seconds();
        *slot += now - t;
        t = now;
    };
    if (warp_failed_) // Phase 8: the GPU failed holding lanes; the world can't go on exactly
        return;
    if (model_ == Model::soup && warp_gpu_.is_open()) {
        // Phase 8: warps on the GPU, between every node's halves -- or, when
        // auto finds it quicker, a tick without the GPU (its lanes home first).
        const bool plain = warp_auto_ && warp_go_plain();
        const double t0 = clock_seconds();
        if (plain) {
            warp_sync();
            if (!warp_failed_)
                pool.run(nodes_, &World::update_task, this);
            ++warp_plain_ticks_;
        } else {
            warp_step(pool);
        }
        if (warp_auto_) { // a mode's recent time: blended while it runs, fresh when it starts again
            const u32 m = plain ? 1 : 0;
            const double s = clock_seconds() - t0;
            warp_tick_s_[m] = warp_last_plain_ == m ? warp_tick_s_[m] * 0.875 + s * 0.125 : s;
            warp_last_plain_ = m;
        }
    } else {
        pool.run(nodes_, &World::update_task, this);
    }
    if (warp_failed_)
        return;
    if (prof_) {
        ++prof_->ticks;
        lap(&prof_->update);
    }
    if (model_ == Model::soup) {
        // Dispersal (Phase 3): departures onto edges, then arrivals due now.
        // Then sum the tallies (arrivals count on their destination), in node
        // order.
        world_corrupted_ = world_transit_lost_ = 0;
        if (accel_async_) {
            accel_barrier();
            lap(prof_ ? &prof_->accel : nullptr);
        }
        if (channels_) {
            scatter_signals();
            lap(prof_ ? &prof_->signals : nullptr);
        }
        if (dispersal_) {
            take_departures();
            lap(prof_ ? &prof_->departures : nullptr);
            deliver_cargo();
            lap(prof_ ? &prof_->delivery : nullptr);
            if (edge_failure_ > 0) {
                links_update();
                lap(prof_ ? &prof_->links : nullptr);
            }
        }
        if (lifecycle_) {
            lifecycle_update();
            lap(prof_ ? &prof_->lifecycle : nullptr);
        }
        static_assert(sizeof(SoupTally) % sizeof(u64) == 0, "SoupTally is all u64");
        soup_last_ = {};
        u64* sum = reinterpret_cast<u64*>(&soup_last_);
        for (u32 n = 0; n < nodes_; ++n) {
            const u64* nt = reinterpret_cast<const u64*>(&soups_[n].tally());
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                sum[i] += nt[i];
        }
        soup_last_.corrupted = world_corrupted_;
        soup_last_.transit_lost = world_transit_lost_;
        soup_last_.collapses = lifecycle_ ? world_collapses_ : 0;
        soup_last_.recoveries = lifecycle_ ? world_recoveries_ : 0;
        lap(prof_ ? &prof_->tally : nullptr);
    } else {
        deliver();
    }
    ++tick_;
    if (landscape_) { // the ports for the tick about to run: bridges, failures, fallow sites
        bridges_update();
        lap(prof_ ? &prof_->ports : nullptr);
    }
}

u64 World::total_units() const {
    u64 sum = 0;
    for (u32 n = 0; n < nodes_; ++n)
        sum += pool_[n];
    return sum;
}

u64 World::hash() const {
    if (model_ == Model::soup) {
        // Phase 8: lanes the GPU holds come back first. That changes where
        // they are, not what they are, so the world stays the same.
        const_cast<World*>(this)->warp_sync();
        Hasher h(0x6E6F76612D736F75ull); // "nova-sou"
        h.word(seed_);
        h.word(tick_);
        h.word(nodes_);
        h.word(soup_size_);
        h.word(edge_src_.size());
        for (usize e = 0; e < edge_src_.size(); ++e)
            h.word((static_cast<u64>(edge_src_[e]) << 32) | edge_dst_[e]);
        if (lifecycle_) { // Phase 4: which sites are nodes, and their lives
            h.bytes(live_.data(), live_.size());
            h.bytes(lived_.data(), lived_.size());
            h.bytes(born_.data(), born_.size() * sizeof(u64));
            h.bytes(fallow_since_.data(), fallow_since_.size() * sizeof(u64));
            h.bytes(life_.data(), life_.size() * sizeof(u32));
        }
        for (u32 n = 0; n < nodes_; ++n)
            soups_[n].hash_into(h);
        if (dispersal_) { // transit; the lineage tags are detector data, not hashed
            h.word(next_seq_);
            h.word(cargo_.size());
            for (usize i = 0; i < cargo_.size(); ++i) {
                const Cargo& c = cargo_[i];
                h.word(c.due);
                h.word(c.seq);
                h.word((static_cast<u64>(c.edge) << 32) | c.len);
                h.word((static_cast<u64>(c.millicores) << 32) | c.gpus);
                h.word(c.npus);
                if (soup_params_.disk) // Phase 4: her disk share
                    h.word(c.disk);
                if (soup_params_.warps) // Phase 8: her form
                    h.word(c.form);
                if (soup_params_.kinds) // Phase 9c: her E millicores, her kind
                    h.word((static_cast<u64>(c.emillicores) << 32) | c.kind);
                h.bytes(cargo_codons_.data() + c.codon_off, c.len);
            }
            if (edge_failure_ > 0) // Phase 4: which links are down, until when
                h.bytes(down_until_.data(), down_until_.size() * sizeof(u64));
        }
        if (channels_) // Phase 4: signals on their way
            h.bytes(pending_.data(), pending_.size() * sizeof(double));
        if (accel_async_) { // Phase 5: accelerator ops in flight
            h.word(acc_ops_.size());
            h.bytes(acc_ops_.data(), acc_ops_.size() * sizeof(AccelOp));
            h.bytes(acc_in_.data(), acc_in_.size());
            h.bytes(acc_addr_.data(), acc_addr_.size() * sizeof(u32));
        }
        return h.finish();
    }
    Hasher h(0x6E6F76612D776F72ull); // "nova-wor"
    h.word(seed_);
    h.word(tick_);
    h.word(nodes_);
    h.word(soup_size_);
    h.word(edge_src_.size());
    for (usize e = 0; e < edge_src_.size(); ++e)
        h.word((static_cast<u64>(edge_src_[e]) << 32) | edge_dst_[e]);
    for (u32 n = 0; n < nodes_; ++n)
        h.word(pool_[n]);
    h.bytes(soup_.data(), soup_.size());
    return h.finish();
}

void World::save_state(ByteWriter& w) const {
    if (model_ == Model::soup) // Phase 8: lanes the GPU holds come back first (see hash())
        const_cast<World*>(this)->warp_sync();
    w.put_u64(tick_);
    w.put_u32(nodes_);
    w.put_u32(soup_size_);
    w.put_u32(edge_count());
    for (usize e = 0; e < edge_src_.size(); ++e) {
        w.put_u32(edge_src_[e]);
        w.put_u32(edge_dst_[e]);
    }
    if (model_ == Model::soup) {
        if (lifecycle_) { // before the soups: their sizes depend on it
            w.put_bytes(live_.data(), live_.size());
            w.put_bytes(lived_.data(), lived_.size());
            for (u32 n = 0; n < nodes_; ++n) {
                w.put_u64(born_[n]);
                w.put_u64(fallow_since_[n]);
                w.put_u32(life_[n]);
            }
            w.put_u64(collapses_total_);
            w.put_u64(recoveries_total_);
        }
        for (u32 n = 0; n < nodes_; ++n)
            soups_[n].save(w);
        if (dispersal_) { // transit, without the lineage tags
            w.put_u64(next_seq_);
            w.put_u32(static_cast<u32>(cargo_.size()));
            for (usize i = 0; i < cargo_.size(); ++i) {
                const Cargo& c = cargo_[i];
                w.put_u64(c.due);
                w.put_u64(c.seq);
                w.put_u32(c.edge);
                w.put_u32(c.len);
                w.put_u32(c.millicores);
                w.put_u32(c.gpus);
                w.put_u32(c.npus);
                if (soup_params_.disk)
                    w.put_u32(c.disk);
                if (soup_params_.warps)
                    w.put_u32(c.form);
                if (soup_params_.kinds) { // Phase 9c
                    w.put_u32(c.emillicores);
                    w.put_u32(c.kind);
                }
                w.put_bytes(cargo_codons_.data() + c.codon_off, c.len);
            }
            if (edge_failure_ > 0)
                for (usize e = 0; e < down_until_.size(); ++e)
                    w.put_u64(down_until_[e]);
        }
        if (channels_)
            w.put_bytes(pending_.data(), pending_.size() * sizeof(double));
        if (accel_async_) {
            w.put_u32(static_cast<u32>(acc_ops_.size()));
            w.put_bytes(acc_ops_.data(), acc_ops_.size() * sizeof(AccelOp));
            w.put_u32(static_cast<u32>(acc_in_.size()));
            w.put_bytes(acc_in_.data(), acc_in_.size());
            w.put_u32(static_cast<u32>(acc_addr_.size()));
            w.put_bytes(acc_addr_.data(), acc_addr_.size() * sizeof(u32));
        }
        return;
    }
    for (u32 n = 0; n < nodes_; ++n)
        w.put_u32(pool_[n]);
    w.put_bytes(soup_.data(), soup_.size());
}

bool World::load_state(ByteReader& r, TextBuf* err) {
    u64 tick = 0;
    u32 nodes = 0, soup_size = 0, edges = 0;
    r.get_u64(&tick);
    r.get_u32(&nodes);
    r.get_u32(&soup_size);
    r.get_u32(&edges);
    if (!r.ok()) {
        err->append("snapshot: world section truncated");
        return false;
    }
    if (nodes != nodes_ || soup_size != soup_size_) {
        err->append("snapshot: world shape does not match its config");
        return false;
    }
    if (static_cast<u64>(edges) * 8 > r.remaining()) {
        err->append("snapshot: edge list truncated");
        return false;
    }
    Array<u64> keys;
    keys.resize(edges);
    for (u32 e = 0; e < edges; ++e) {
        u32 src = 0, dst = 0;
        r.get_u32(&src);
        r.get_u32(&dst);
        keys[e] = (static_cast<u64>(src) << 32) | dst;
    }
    if (model_ == Model::soup) {
        if (!r.ok()) {
            err->append("snapshot: invalid edge list");
            return false;
        }
        // The lifecycle (Phase 4): which sites are nodes and their lives come
        // first. Each node in a later life gets its params from the fields at
        // the tick it appeared and a soup of that shape (load fills it), and
        // the graph is the one among the live sites.
        if (lifecycle_) {
            if (r.remaining() < static_cast<u64>(nodes_) * 22 + 16) {
                err->append("snapshot: lifecycle section truncated");
                return false;
            }
            r.get_bytes(live_.data(), nodes_);
            r.get_bytes(lived_.data(), nodes_);
            for (u32 n = 0; n < nodes_; ++n) {
                r.get_u64(&born_[n]);
                r.get_u64(&fallow_since_[n]);
                r.get_u32(&life_[n]);
            }
            r.get_u64(&collapses_total_);
            r.get_u64(&recoveries_total_);
            Array<double> z[kFieldCount];
            for (u32 n = 0; n < nodes_; ++n) {
                if (life_[n] == 0)
                    continue;
                fields_at(born_[n], z);
                derive_node(n, z, false);
                soups_[n].init(&node_params_[n], seed_, n, life_[n]);
            }
            if (!landscape_) { // a landscape's paths are fixed: only the procedural graph is rebuilt
                assemble_graph(seed_, gp_, &pg_, live_.data());
                if (!build_edges(pg_.keys.data(), pg_.keys.size())) {
                    err->append("snapshot: invalid live graph");
                    return false;
                }
                edge_latency_.assign(pg_.latency.data(), pg_.latency.size());
                edge_bandwidth_.assign(pg_.bandwidth.data(), pg_.bandwidth.size());
                edge_dist_.assign(pg_.dist.data(), pg_.dist.size());
            }
        }
        // The edges' lengths, numbering and ports derive from the config's
        // graph, so the snapshot's edges must be exactly those.
        bool same = keys.size() == edge_src_.size();
        for (usize e = 0; same && e < keys.size(); ++e)
            same = keys[e] == ((static_cast<u64>(edge_src_[e]) << 32) | edge_dst_[e]);
        if (!same) {
            err->append("snapshot: edge list does not match its config");
            return false;
        }
        for (u32 n = 0; n < nodes_; ++n)
            if (!soups_[n].load(r, err))
                return false;
        cargo_.clear();
        cargo_codons_.clear();
        next_seq_ = 0;
        if (dispersal_) {
            u32 count = 0;
            r.get_u64(&next_seq_);
            r.get_u32(&count);
            for (u32 i = 0; i < count && r.ok(); ++i) {
                Cargo c = {};
                r.get_u64(&c.due);
                r.get_u64(&c.seq);
                r.get_u32(&c.edge);
                r.get_u32(&c.len);
                r.get_u32(&c.millicores);
                r.get_u32(&c.gpus);
                r.get_u32(&c.npus);
                if (soup_params_.disk)
                    r.get_u32(&c.disk);
                if (soup_params_.warps)
                    r.get_u32(&c.form);
                if (soup_params_.kinds) { // Phase 9c
                    r.get_u32(&c.emillicores);
                    r.get_u32(&c.kind);
                }
                if (!r.ok() || c.edge >= edge_src_.size() || c.len == 0 || c.len > r.remaining() ||
                    c.due < tick || c.seq >= next_seq_ || c.form > kFormGpu || c.kind > kKindE) {
                    err->append("snapshot: cargo out of range");
                    return false;
                }
                const u32 bw = edge_bandwidth_[c.edge];
                c.ticks = (c.len + bw - 1) / bw + edge_latency_[c.edge]; // as when sent
                c.codon_off = static_cast<u32>(cargo_codons_.size());
                cargo_codons_.resize(cargo_codons_.size() + c.len);
                r.get_bytes(cargo_codons_.data() + c.codon_off, c.len);
                for (u32 k = 0; k < c.len; ++k) {
                    if (cargo_codons_[c.codon_off + k] > 63) {
                        err->append("snapshot: cargo codon out of range");
                        return false;
                    }
                }
                cargo_.push(c);
            }
            if (!r.ok()) {
                err->append("snapshot: cargo section truncated");
                return false;
            }
        }
        down_until_.resize(edge_src_.size());
        down_until_.fill(0);
        if (dispersal_ && edge_failure_ > 0) {
            for (usize e = 0; e < down_until_.size(); ++e)
                r.get_u64(&down_until_[e]);
            if (!r.ok()) {
                err->append("snapshot: link section truncated");
                return false;
            }
        }
        if (channels_) {
            const usize bytes = pending_.size() * sizeof(double);
            if (r.remaining() < bytes) {
                err->append("snapshot: signal section truncated");
                return false;
            }
            r.get_bytes(pending_.data(), bytes);
        }
        acc_ops_.clear();
        acc_in_.clear();
        acc_addr_.clear();
        if (accel_async_) {
            u32 no = 0, ni = 0, na = 0;
            bool ok = r.get_u32(&no) && static_cast<u64>(no) * sizeof(AccelOp) <= r.remaining();
            if (ok) {
                acc_ops_.resize(no);
                ok = r.get_bytes(acc_ops_.data(), static_cast<usize>(no) * sizeof(AccelOp));
            }
            ok = ok && r.get_u32(&ni) && ni <= r.remaining();
            if (ok) {
                acc_in_.resize(ni);
                ok = r.get_bytes(acc_in_.data(), ni);
            }
            ok = ok && r.get_u32(&na) && static_cast<u64>(na) * 4 <= r.remaining();
            if (ok) {
                acc_addr_.resize(na);
                ok = r.get_bytes(acc_addr_.data(), static_cast<usize>(na) * 4);
            }
            for (usize i = 0; ok && i < acc_ops_.size(); ++i) {
                const AccelOp& op = acc_ops_[i];
                const u64 reads = static_cast<u64>(op.src_n) + op.dst_n + op.mat_n;
                ok = op.node < nodes_ && op.kind <= kAccArgmax && op.in_off + reads <= ni &&
                     static_cast<u64>(op.addr_off) + op.out_n <= na && op.due >= tick;
            }
            if (!ok) {
                err->append("snapshot: accelerator section truncated or out of range");
                return false;
            }
        }
        // Ops that were on a device before the snapshot finish on the CPU
        // reference: the same results.
        acc_job_.resize(acc_ops_.size());
        acc_job_.fill(-1);
        acc_slot_.resize(acc_ops_.size());
        acc_slot_.fill(0);
        acc_held_.clear();
        acc_held_scalar_.resize(acc_ops_.size());
        acc_held_scalar_.fill(0);
        acc_held_flags_.resize(acc_ops_.size());
        acc_held_flags_.fill(0);
        init_ports(); // recounts in_flight_ from the cargo, derives the down flags
        for (u32 e = 0; e < in_flight_.size(); ++e) {
            if (in_flight_[e] > edge_capacity_ && dispersal_) {
                err->append("snapshot: an edge carries more than its capacity");
                return false;
            }
        }
        if (lineages_) // detector state is not in snapshots: start afresh
            for (u32 n = 0; n < nodes_; ++n)
                soups_[n].set_observer(&lineages_[n]);
        tick_ = tick;
        soup_last_ = {};
        if (landscape_) // the bridges as they stand at the snapshot's tick
            refresh_ports();
        return true;
    }
    Array<u32> pools;
    pools.resize(nodes);
    for (u32 n = 0; n < nodes; ++n)
        r.get_u32(&pools[n]);
    if (!r.ok() || r.remaining() < soup_.size()) {
        err->append("snapshot: world section truncated");
        return false;
    }
    if (!build_edges(keys.data(), keys.size())) {
        err->append("snapshot: invalid edge list");
        return false;
    }
    r.get_bytes(soup_.data(), soup_.size());
    for (usize i = 0; i < soup_.size(); ++i) {
        if (soup_[i] > 63) {
            err->append("snapshot: codon out of range");
            return false;
        }
    }
    pool_.assign(pools.data(), pools.size());
    tick_ = tick;
    last_ = {};
    return true;
}

} // namespace nova
