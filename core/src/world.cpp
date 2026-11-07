#include "nova/world.h"

#include <cmath>  // floor
#include <cstdio> // snprintf
#include <cstring> // strlen, memcmp

#include "nova/hash.h"
#include "nova/keys.h" // the settings registry
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

// A live knob's value, checked against its registry entry (keys.inc), as
// World::init's read checks it.
KnobResult knob_f(Str key, Str v, double* out) {
    return key_check(key, v, KeyAt{}, out) ? KnobResult::ok : KnobResult::bad_value;
}

KnobResult knob_u(Str key, Str v, KeyAt at, u32* out) {
    double d = 0;
    if (!key_check(key, v, at, &d))
        return KnobResult::bad_value;
    *out = static_cast<u32>(d);
    return KnobResult::ok;
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
        return knob_f(key, v, &out->f);
    case Knob::heat_ambient:
        return knob_f(key, v, &out->f);
    case Knob::none:
        return is_structural(key) ? KnobResult::not_live : KnobResult::unknown_key;
    case Knob::send_rate:
        return knob_f(key, v, &out->f);
    case Knob::sends_per_tick:
    case Knob::mutations_per_tick:
    case Knob::max_parcel:
        return knob_u(key, v, KeyAt{}, &out->u);
    case Knob::spark_rate:
        return knob_f(key, v, &out->f);
    case Knob::spark_grant:
        return knob_u(key, v, KeyAt{}, &out->u);
    case Knob::spark_min:
    case Knob::spark_max:
        // Each length is checked on its own; a min above the max (for a while,
        // between two changes) is read as the range between them.
        return knob_u(key, v, key_max(soup_size), &out->u);
    }
    return KnobResult::unknown_key;
}

const char* const kKnobKeys[] = {"demo.send_rate", "demo.sends_per_tick", "demo.mutations_per_tick",
                                 "demo.max_parcel"};

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
    seed_ = key_u64(cfg, "seed");
    nodes_ = key_u32(cfg, "world.nodes");
    Str graph = key_choice(cfg, "world.graph");
    procedural_ = graph.eq("procedural");
    landscape_ = graph.eq("landscape");
    spatial_ = procedural_ || landscape_;
    u32 chords = spatial_ ? 0 : key_u32(cfg, "world.chords", key_default(nodes_ / 2));
    GraphParams gp;
    gp.nodes = nodes_;
    if (procedural_) {
        gp.links = key_u32(cfg, "world.links");
        gp.ruggedness = key_f64(cfg, "world.ruggedness");
        gp.long_links = key_f64(cfg, "world.long_links");
        gp.latency_scale = key_f64(cfg, "edge.latency_scale");
        gp.bandwidth = key_u32(cfg, "edge.bandwidth");
    }
    if (landscape_) // Phase 7 (design.md 6.13): one world generated like a game level
        read_land_params(cfg);
    soup_size_ = key_u32(cfg, "world.soup_size");
    model_ = key_choice(cfg, "world.model").eq("soup") ? Model::soup : Model::demo;
    u32 pool_initial = 0;
    if (model_ == Model::demo) {
        pool_capacity_ = key_u32(cfg, "world.pool_capacity");
        pool_initial = key_u32(cfg, "world.pool_initial", key_default_max(pool_capacity_ / 2, pool_capacity_));
    }
    outbox_capacity_ = key_u32(cfg, "world.outbox_capacity");

    if (model_ == Model::demo) {
        for (usize i = 0; i < sizeof kKnobKeys / sizeof kKnobKeys[0]; ++i) {
            Str key = kKnobKeys[i];
            Str v = key_text(cfg, key); // (checked as a live knob: set_knob)
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
                    vary[f] = key_f64(cfg, kVaryKeys[f]);
        if (landscape_)
            read_spreads(cfg);
        one_origin = key_choice(cfg, "world.origins").eq("one");
        Str sn = key_str(cfg, "seed.node");
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
                land_ = key_f64(cfg, "world.land"); // Phase 10: every feature on by default
                drift_ = key_f64(cfg, "world.drift");
            }
            budget = key_u32(cfg, "world.budget", key_max(nodes_));
            recover_every_ = key_u32(cfg, "node.recover_every");
            fallow_min_ = key_u64(cfg, "node.fallow_min");
        }
        lifecycle_ = soup_params_.endurance > 0 || land_ < 1.0;
        if (lifecycle_ && !spatial_)
            cfg.fail("node.endurance", "the node lifecycle needs world.graph = procedural or landscape");
        budget_ = budget;
        // Phase 4: the signal channel's world-level settings.
        channels_ = soup_params_.signal_channels;
        attenuation_ = 0.25;
        reach_ = 8;
        if (channels_) {
            attenuation_ = key_f64(cfg, "signal.attenuation");
            reach_ = key_u32(cfg, "signal.reach");
        }
        if (dispersal_) {
            edge_capacity_ = key_u32(cfg, "edge.capacity");
            if (!spatial_)
                gp.bandwidth = key_u32(cfg, "edge.bandwidth");
            // Phase 4 (design.md 6.10): transit corruption and link failures.
            edge_corruption_ = key_f64(cfg, "edge.corruption");
            edge_failure_ = key_f64(cfg, "edge.failure");
            edge_downtime_ = key_f64(cfg, "edge.downtime");
        }
    }
    if (landscape_ && model_ != Model::soup)
        cfg.fail("world.graph", "a landscape needs world.model = soup");
    // Phase 7b: the disk tier on real disk, in a folder of this run's own
    // under disk.path (its files go when the run ends).
    disk_folder_.clear();
    if (model_ == Model::soup) {
        const Str dp = key_str(cfg, "disk.path");
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
    for (usize i = 0; i < cargo_.size(); ++i) // (dormant programs travel outside the capacity)
        in_flight_[cargo_[i].edge] += cargo_[i].dormant ? 0 : 1;
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
    p.aspect = key_f64(cfg, "land.aspect");
    p.fraction = key_f64(cfg, "land.fraction");
    p.scale = key_f64(cfg, "land.scale");
    p.warp = key_f64(cfg, "land.warp");
    p.border = key_f64(cfg, "land.border");
    p.resolution = key_u32(cfg, "land.resolution");
    p.paths_mean = key_f64(cfg, "paths.mean");
    p.paths_spread = key_f64(cfg, "paths.spread");
    p.paths_isolated = key_f64(cfg, "paths.isolated");
    p.paths_hubs = key_f64(cfg, "paths.hubs");
    p.paths_max = key_u32(cfg, "paths.max");
    p.paths_connected = key_flag(cfg, "paths.connected");
    p.paths_long = key_f64(cfg, "paths.long");
    p.latency_scale = key_f64(cfg, "edge.latency_scale");
    p.bandwidth = key_u32(cfg, "edge.bandwidth");
    p.bridge_count = key_u32(cfg, "bridge.count");
    p.bridge_isolated = key_f64(cfg, "bridge.isolated");
    p.bridge_max_length = key_f64(cfg, "bridge.max_length");
    p.bridge_latency_scale = key_f64(cfg, "bridge.latency_scale");
    p.bridge_bandwidth = key_u32(cfg, "bridge.bandwidth");
    p.open_every = key_u32(cfg, "bridge.open_every");
    p.open_ticks = key_u32(cfg, "bridge.open_ticks");
    const Str ph = key_choice(cfg, "bridge.phase");
    if (ph.eq("together"))
        p.phase = kPhaseTogether;
    else if (ph.eq("random"))
        p.phase = kPhaseRandom;
    else
        p.phase = kPhaseStaggered;
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
        // The <spread>.* family (keys.inc).
        s.spread = key_f64(cfg, key("spread"));
        s.smooth = key_f64(cfg, key("smooth"));
        s.barren = key_f64(cfg, key("barren"));
        s.hotspots = key_u32(cfg, key("hotspots"));
        s.boost = key_f64(cfg, key("hotspot_boost"));
        // Phase 8: rich where another distribution is poor (its noise field
        // the opposite way round), so GPU-rich regions can be CPU-poor.
        const Str a = key_choice(cfg, key("against"));
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

u64 World::arrived_dry_sum() const {
    u64 s = 0;
    for (u32 n = 0; n < nodes_; ++n)
        s += soups_[n].arrived_dry();
    return s;
}

u64 World::arrived_sum() const {
    u64 s = 0;
    for (u32 n = 0; n < nodes_; ++n)
        s += soups_[n].arrived_all();
    return s;
}

// The barrier (Phase 4 lifecycle, design.md 6.10). In node order, each node
// whose tick found it worn out collapses: every body dies an
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
        collapse_age_ += tick_ - born_[n]; // (diagnostics only: how old, and how many bodies it took)
        collapse_bodies_ += soups_[n].body_count();
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
    const Str g = key_str(cfg, "accel.gpu");
    const Str npu = key_choice(cfg, "accel.npu");
    verify_every_ = key_u32(cfg, "accel.verify");
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
    warp_gpu_bodies_ = warp_ref_bodies_ = 0;
    warp_compile_ms_ = 0;
    warp_failed_ = false;
    warp_limit_ = 1000000000u; // the search starts with every node on the GPU, and probes down
    warp_dir_ = 1;
    warp_stride_ = 1.0;
    warp_cost_ = 0;
    warp_cost_tick_ = 0;
    warp_climb_fresh_ = true;
    warp_gstep_ = warp_hstep_ = 0;
    warp_auto_ticks_ = warp_plain_ticks_ = 0;
    warp_tick_s_[0] = warp_tick_s_[1] = 0;
    warp_last_plain_ = 2;
    const Str d = key_choice(cfg, "warp.device");
    warp_auto_ = d.eq("auto");
    warp_mixed_ = d.eq("mixed"); // a test: nodes split between the two at random, every round
    warp_verify_ = key_u32(cfg, "warp.verify");
    // warp.kernel (docs/dxc-plan.md D3): DXC's kernel where it can run (32
    // lanes, Shader Model 6.0), else FXC's; or FXC's always. The same words.
    const Str kern = key_choice(cfg, "warp.kernel");
    if (!soup_params_.warps || d.eq("cpu"))
        return;
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    u32 pick = kNone;
    for (u32 i = 0; i < n && pick == kNone; ++i)
        if (devs[i].gpu && devs[i].hardware)
            pick = i;
    TextBuf why;
    WarpKernelOptions kopt;
    kopt.fxc = kern.eq("fxc");
    if (pick == kNone) {
        warp_notes_.append("no hardware GPU found: warps run on the CPU reference\n");
    } else if (!warp_gpu_.open(pick, soup_params_.warp_lanes, &why, &warp_compile_ms_, &kopt)) {
        warp_notes_.append("GPU ");
        warp_notes_.append(devs[pick].name);
        warp_notes_.append(" failed to start warps (");
        warp_notes_.append(why.view());
        warp_notes_.append("): warps run on the CPU reference\n");
    } else {
        warp_notes_.append("warps on ");
        warp_notes_.append(devs[pick].name);
        warp_notes_.append(": ");
        warp_notes_.append(warp_gpu_.kernel());
        warp_notes_.append("\n");
    }
}

// warp.device = auto, tick by tick: with the GPU (the split rounds of
// warp_step) or without it (every node on the reference, as warp.device =
// cpu), whichever has taken less time lately. It starts with five ticks of
// each; after that it tries the one not in use again, as the world changes,
// for three ticks in a row: every 25th tick while the two are within a
// quarter of each other, every 100th while one is up to twice as quick, every
// 400th beyond. The first tick after a switch isn't timed (step()): it pays
// for the switch itself (every changed warp's lanes crossing over), and a
// one-tick probe judged by it never switched back (Phase 10 profiling: a
// landscape stuck 12,000 ticks without the GPU at four times the cost). The
// choice never changes the world.
bool World::warp_go_plain() {
    const u64 k = warp_auto_ticks_++;
    if (k < 10)
        return k >= 5;
    if (warp_probe_left_ > 0) { // finishing a probe
        --warp_probe_left_;
        return warp_probe_plain_;
    }
    const double a = warp_tick_s_[0], b = warp_tick_s_[1];
    const bool plain = b < a;
    const double ratio = plain ? (b > 0 ? a / b : 1e9) : (a > 0 ? b / a : 1e9);
    const u64 every = ratio < 1.25 ? 25 : (ratio < 2.0 ? 100 : 400);
    if (k % every == 0) { // a probe of the other way: three ticks
        warp_probe_plain_ = !plain;
        warp_probe_left_ = 2;
        return !plain;
    }
    return plain;
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
    const double t0 = clock_seconds(); // (each node's time: the slowest go first next tick)
    if (!w->live_.empty() && !w->live_[node]) { // fallow (Phase 4): nothing runs
        w->soups_[node].idle(w->tick_);
        w->warp_active_[node] = 0;
        w->warp_node_t_[3 * node] = clock_seconds() - t0;
        return;
    }
    if (w->channels_)
        w->signal_field(node);
    w->soups_[node].tick_first(w->tick_);
    w->warp_active_[node] = 1;
    w->warp_node_t_[3 * node] = clock_seconds() - t0;
}

void World::warp_pack_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    const double t0 = clock_seconds();
    if (w->warp_active_[node])
        w->warp_active_[node] = w->soups_[node].warp_pack(w->warp_round_) ? 1 : 0;
    if (w->warp_round_ == 0)
        w->warp_node_t_[3 * node + 1] = clock_seconds() - t0;
}

// The split's choice for node n's bodies this round (warp_step): the GPU or
// the CPU threads. (Keeping a body on the GPU until its guess passed the
// limit by a quarter, to save the fetch a move costs, lengthened the kernel
// more than it saved: bodies grow past the limit, they don't hover there.)
void World::warp_choose_task(void* ctx, u32 n) {
    World* w = static_cast<World*>(ctx);
    u32 gpu_n = 0, ref_n = 0, guess = 0, longest = 0;
    w->warp_on_gpu_[n] = 0;
    if (w->warp_active_[n]) {
        Soup& s = w->soups_[n];
        Array<u8>& on = s.warp_record_on_gpu();
        const u64 r = w->warp_round_;
        for (usize i = 0; i < on.size(); ++i) {
            const u32 chain = s.warp_record_chain(i);
            longest = chain > longest ? chain : longest;
            bool gpu = w->warp_use_gpu_ && w->warp_same_ && (!w->warp_auto_ || chain <= w->warp_limit_);
            if (w->warp_mixed_ && w->warp_use_gpu_) // the test mode: lanes cross between the two all the time
                gpu = w->warp_same_ &&
                      (mix64((w->tick_ << 20) ^ (r << 16) ^ (static_cast<u64>(i) << 40) ^ n ^ w->seed_) & 1);
            on[i] = gpu ? 1 : 0;
            if (gpu) {
                ++gpu_n;
                w->warp_on_gpu_[n] = 1;
                guess = chain > guess ? chain : guess;
            } else {
                ++ref_n;
            }
        }
    }
    w->warp_node_gpu_[n] = gpu_n;
    w->warp_node_ref_[n] = ref_n;
    w->warp_node_guess_[n] = guess;
    w->warp_node_longest_[n] = longest;
}

// The round's lists for node n (warp_step), counted, then filled at the
// node's offsets: lanes to fetch (Device lanes of bodies the CPU runs, or of
// every body when verifying), lanes to send (Host lanes of bodies the GPU
// runs), and the records the GPU runs with their sizes.
void World::warp_list_count_task(void* ctx, u32 n) {
    World* w = static_cast<World*>(ctx);
    u32 nf = 0, ns = 0, nr = 0;
    if (w->warp_active_[n]) {
        const Soup& s = w->soups_[n];
        const u32 L = s.lanes_per_warp();
        const u32* I = s.warp_image_view().data();
        const Array<u8>& on = w->soups_[n].warp_record_on_gpu();
        for (usize k = 0; k < s.warp_records().size(); ++k) {
            const bool gpu = on[k] != 0;
            nr += gpu ? 1 : 0;
            const u32 rec = s.warp_records()[k];
            for (u32 q = 0; q < I[rec + WB_NWARP]; ++q) {
                const u8 st = s.lane_state(I[rec + WB_WARPS + q * WW_SIZE + WW_LANE0] / L);
                nf += ((!gpu || w->warp_verifying_) && st == Soup::kLanesDevice) ? 1 : 0;
                ns += (gpu && st == Soup::kLanesHost) ? 1 : 0;
            }
        }
    }
    w->warp_node_nfetch_[n] = nf;
    w->warp_node_nsend_[n] = ns;
    w->warp_node_nrec_[n] = nr;
}

void World::warp_list_fill_task(void* ctx, u32 n) {
    World* w = static_cast<World*>(ctx);
    if (!w->warp_active_[n])
        return;
    Soup& s = w->soups_[n];
    const u32 L = s.lanes_per_warp();
    const u32* I = s.warp_image_view().data();
    const Array<u8>& on = s.warp_record_on_gpu();
    u32 f = w->warp_node_ofetch_[n], sd = w->warp_node_osend_[n], rc = w->warp_node_orec_[n];
    for (usize k = 0; k < s.warp_records().size(); ++k) {
        const bool gpu = on[k] != 0;
        const u32 rec = s.warp_records()[k];
        if (gpu) {
            w->warp_job_recs_[rc] = rec;
            w->warp_job_words_[rc] = s.warp_record_words(k);
            w->warp_job_chain_[rc] = s.warp_record_chain(k); // (long bodies go first on the GPU)
            ++rc;
        }
        for (u32 q = 0; q < I[rec + WB_NWARP]; ++q) {
            const u32 slot = I[rec + WB_WARPS + q * WW_SIZE + WW_LANE0] / L;
            const u8 st = s.lane_state(slot);
            if ((!gpu || w->warp_verifying_) && st == Soup::kLanesDevice) {
                w->warp_fetch_[f] = WarpFetch{n, slot, s.lanes_mut().data() + static_cast<usize>(slot) * L};
                w->warp_fetch_node_[f] = n;
                ++f;
            }
            if (gpu && st == Soup::kLanesHost)
                w->warp_send_[sd++] = slot;
        }
    }
}

// warp.verify: every active node's bodies on the reference, one after another.
void World::warp_ref_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node])
        w->soups_[node].warp_run_ref();
}

// The records the reference runs this round (the bodies the GPU doesn't
// take), the longest bodies (most steps in their last first round) first, so
// they start early and the rest fill the threads around them instead of a
// long one holding up the round at its end.
void World::warp_ref_build() {
    warp_ref_list_.clear();
    Array<u64> order; // (chain << 32 | place in `pairs`), sorted ascending, taken from the end
    Array<u64> pairs; // (node << 32 | record)
    for (u32 n = 0; n < nodes_; ++n) {
        if (!warp_active_[n])
            continue;
        const Soup& s = soups_[n];
        const Array<u8>& on = soups_[n].warp_record_on_gpu();
        for (usize r = 0; r < on.size(); ++r) {
            if (on[r])
                continue;
            const u64 c = s.warp_record_chain(r);
            order.push((c > 0xFFFFFFFFull ? 0xFFFFFFFFull : c) << 32 | pairs.size());
            pairs.push((static_cast<u64>(n) << 32) | r);
        }
    }
    sort_u64(order.data(), order.size());
    for (usize j = order.size(); j-- > 0;)
        warp_ref_list_.push(pairs[static_cast<usize>(order[j] & 0xFFFFFFFFu)]);
    warp_ref_time_.resize(warp_ref_list_.size());
}

void World::warp_ref_rec_task(void* ctx, u32 i) {
    World* w = static_cast<World*>(ctx);
    const u64 e = w->warp_ref_list_[i];
    const double t0 = clock_seconds(); // (auto's model needs the reference's time a step)
    w->soups_[static_cast<u32>(e >> 32)].warp_run_record(static_cast<usize>(e & 0xFFFFFFFFu));
    w->warp_ref_time_[i] = clock_seconds() - t0;
}

// auto's split, from a model (Phase 10 profiling). The search for a limit
// compared rounds' wall times, which wander with whatever else uses the
// machine (a game sharing the GPU kept it a factor of two off). Instead two
// rates are measured: the GPU's time a serial step (its kernel time on its own
// clock over the longest body it ran: it runs a body's steps one after
// another, and that body sets the kernel's length) and the reference's time a
// warp step on one thread. With the bodies' chains from their last first
// round, longest first, sending the k longest to the CPU threads takes them
// max(longest x h, sum x h / threads), and leaves the GPU the (k+1)th's chain
// x g; the limit is the chain that makes the slower of the two quickest.
u32 World::warp_model_limit(u32 threads) {
    // Only the long bodies matter (the crossing is among the few longest), so
    // only chains above a quarter of the last limit are gathered and sorted,
    // the rest standing for "at most that"; all of them if it isn't there.
    for (u32 pass = 0; pass < 2; ++pass) {
        const u64 floor = pass == 0 ? warp_limit_ / 4 : 0;
        Array<u64>& c = warp_chains_; // (chain), ascending: read from the end
        c.clear();
        for (u32 n = 0; n < nodes_; ++n) {
            if (!warp_active_[n])
                continue;
            const Soup& s = soups_[n];
            for (usize i = 0; i < s.warp_record_count(); ++i) {
                const u64 ch = s.warp_record_chain(i);
                if (ch > floor)
                    c.push(ch);
            }
        }
        if (c.empty() && pass == 0)
            continue;
        if (c.empty())
            return warp_limit_;
        sort_u64(c.data(), c.size());
        u32 lim = 0;
        if (warp_model_pick(threads, static_cast<double>(floor), &lim) || pass == 1)
            return lim;
    }
    return warp_limit_;
}

// The pick over warp_chains_ (sorted; the bodies not gathered run at most
// `rest` steps): false when the best is to send every gathered body to the
// CPU and some weren't gathered (then it needs them all).
bool World::warp_model_pick(u32 threads, double rest, u32* limit) {
    const Array<u64>& c = warp_chains_;
    const usize nb = c.size();
    auto at = [&](usize k) { return static_cast<double>(c[nb - 1 - k]); }; // the kth longest
    const double g = warp_gstep_, h = warp_hstep_, t = threads ? static_cast<double>(threads) : 1.0;
    double sum = 0, best = 1e300;
    usize best_k = 0;
    for (usize k = 0; k <= nb; ++k) {
        const double gpu = (k < nb ? at(k) : rest) * g;
        const double cpu = k > 0 ? (at(0) * h > sum * h / t ? at(0) * h : sum * h / t) : 0.0;
        const double slower = gpu > cpu ? gpu : cpu;
        if (slower < best) {
            best = slower;
            best_k = k;
        }
        if (cpu > gpu) // past the crossing the CPU only gets slower
            break;
        if (k < nb)
            sum += at(k);
    }
    if (best_k >= nb) {
        *limit = static_cast<u32>(rest); // every gathered body to the CPU
        return rest == 0;
    }
    const double lim = at(best_k);
    *limit = lim > 1e9 ? 1000000000u : static_cast<u32>(lim);
    return true;
}

// The model's two rates, after a round (smoothed: a quarter new).
void World::warp_model_measure(u32 round, bool cpu_ran) {
    if (round == 0 && !warp_jobs_.empty()) { // the GPU's time a serial step
        // In the guesses' own units (the longest guess it was given, not the
        // longest it ran): a body may run longer than its guess, and the pick
        // compares guesses, so their bias cancels.
        const double ms = warp_gpu_.last_kernel_ms();
        const u64 per = warp_gpu_guess_ >= 200 ? warp_gpu_guess_ : warp_gpu_.last_longest();
        if (ms > 0 && per >= 200) { // (a short longest body would be mostly fixed costs)
            const double g = ms / 1000.0 / static_cast<double>(per);
            warp_gstep_ = warp_gstep_ > 0 ? warp_gstep_ * 0.75 + g * 0.25 : g;
        }
    }
    if (cpu_ran && warp_ref_time_.size() == warp_ref_list_.size()) { // the reference's, a step on a thread
        double secs = 0;
        u64 steps = 0;
        for (usize i = 0; i < warp_ref_list_.size(); ++i) {
            secs += warp_ref_time_[i];
            const u64 e = warp_ref_list_[i];
            const Soup& s = soups_[static_cast<u32>(e >> 32)];
            const u32* I = s.warp_image_view().data();
            const u32 rec = s.warp_records()[static_cast<usize>(e & 0xFFFFFFFFu)];
            for (u32 q = 0; q < I[rec + WB_NWARP]; ++q)
                steps += I[rec + WB_WARPS + q * WW_SIZE + WW_STEPS];
        }
        if (steps >= 10000) {
            const double h = secs / static_cast<double>(steps);
            warp_hstep_ = warp_hstep_ > 0 ? warp_hstep_ * 0.75 + h * 0.25 : h;
        }
    }
}

void World::warp_unpack_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (!w->warp_active_[node])
        return;
    if (w->warp_verifying_) // the reference's result was kept: every lane is the host's
        w->soups_[node].warp_record_on_gpu().fill(0);
    w->soups_[node].warp_unpack(); // the bodies the GPU ran hand it their lanes
}

void World::warp_host_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    if (w->warp_active_[node])
        w->soups_[node].warp_host(w->warp_round_);
}

void World::warp_second_task(void* ctx, u32 node) {
    World* w = static_cast<World*>(ctx);
    const double t0 = clock_seconds();
    if (w->live_.empty() || w->live_[node])
        w->soups_[node].tick_second();
    w->warp_node_t_[3 * node + 2] = clock_seconds() - t0;
}

// set_warp_dump's file, little-endian 32-bit words: "NWRD" (0x4452574E), the
// version (1), lanes a warp, the constants' count, the jobs' count, the
// constants; then per job its node, image words, records and lanes (Lane
// count), the image, the record offsets, each record's words, and the lanes.
bool World::warp_write_dump(const Array<u32>& cst) {
    FILE* f = fopen(warp_dump_path_, "wb");
    if (!f)
        return false;
    bool ok = true;
    auto put = [&](const void* p, usize words) {
        ok = ok && (words == 0 || fwrite(p, 4, words, f) == words);
    };
    const u32 head[5] = {0x4452574Eu, 1u, soup_params_.warp_lanes, static_cast<u32>(cst.size()),
                         static_cast<u32>(warp_jobs_.size())};
    put(head, 5);
    put(cst.data(), cst.size());
    Array<u32> words;
    for (usize i = 0; i < warp_jobs_.size(); ++i) {
        const WarpJob& j = warp_jobs_[i];
        const u32 jh[4] = {j.node, j.img_words, j.nrec, j.nlanes};
        put(jh, 4);
        put(j.img, j.img_words);
        put(j.recs, j.nrec);
        words.resize(j.nrec);
        for (u32 r = 0; r < j.nrec; ++r)
            words[r] = warp_job_rec_words(j, r);
        put(words.data(), j.nrec);
        put(j.lanes, static_cast<usize>(j.nlanes) * (sizeof(Lane) / 4));
    }
    ok = fclose(f) == 0 && ok;
    return ok;
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
void World::warp_step(ThreadPool& pool, bool use_gpu) {
    double cpu_s = 0, gpu_s = 0, pack_s = 0;
    // Profiling: each phase's wall time (the first round's apart from later ones').
    double tw = prof_ ? clock_seconds() : 0;
    auto wmark = [&](double StepProfile::*part) {
        if (!prof_)
            return;
        const double now = clock_seconds();
        prof_->*part += now - tw;
        tw = now;
    };
    warp_active_.resize(nodes_);
    warp_on_gpu_.resize(nodes_);
    if (warp_node_t_.size() != static_cast<usize>(nodes_) * 3) {
        warp_node_t_.resize(static_cast<usize>(nodes_) * 3);
        warp_node_t_.fill(0.0);
    }
    pool.run(nodes_, &World::warp_first_task, this);
    wmark(&StepProfile::w_first);
    for (u32 r = 0; r < soup_params_.warp_rounds && !warp_failed_; ++r) {
        warp_round_ = r;
        double t = clock_seconds();
        tw = t;
        pool.run(nodes_, &World::warp_pack_task, this);
        wmark(r == 0 ? &StepProfile::w_pack : &StepProfile::w_later);
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
        // Which bodies the GPU takes. A GPU runs a body's steps one after
        // another, so under auto a body whose last first round ran more than
        // warp_limit_ steps goes to the CPU threads, and the rest of its
        // node's bodies stay on the GPU (Phase 10 profiling: it was decided
        // per node, so one long body either sent its whole node to the CPU or
        // held up the GPU's round). The two run in the same node's image and
        // lanes at once: a body's record and warps are its own.
        if (use_gpu && warp_auto_ && r == 0 && warp_gstep_ > 0 && warp_hstep_ > 0)
            warp_limit_ = warp_model_limit(pool.threads());
        // Each node chooses on a thread (warp_choose_task); the sums after.
        warp_use_gpu_ = use_gpu;
        warp_same_ = same;
        warp_node_gpu_.resize(nodes_);
        warp_node_ref_.resize(nodes_);
        warp_node_guess_.resize(nodes_);
        warp_node_longest_.resize(nodes_);
        pool.run(nodes_, &World::warp_choose_task, this);
        u32 gpu_recs = 0, ref_recs = 0, longest_all = 0;
        if (r == 0)
            warp_gpu_guess_ = 0;
        for (u32 n = 0; n < nodes_; ++n) {
            gpu_recs += warp_node_gpu_[n];
            ref_recs += warp_node_ref_[n];
            longest_all = warp_node_longest_[n] > longest_all ? warp_node_longest_[n] : longest_all;
            if (r == 0 && warp_node_guess_[n] > warp_gpu_guess_)
                warp_gpu_guess_ = warp_node_guess_[n];
        }
        if (prof_ && r == 0)
            prof_->w_choose += clock_seconds() - tw;
        // warp.verify: this round on both, compared (the GPU's bodies' inputs
        // must then be on the host too).
        // (set_warp_dump's round is verified too, so its lanes are all on the host.)
        const bool dump_now = warp_dump_path_ && r == 0 && gpu_recs && tick_ + 1 >= warp_dump_tick_;
        warp_verifying_ = (warp_verify_ && gpu_recs && (warp_gpu_rounds_ % warp_verify_ == 0)) || dump_now;
        // The lists, built per node on the threads, in two passes (count, then
        // fill at each node's offset), so they come out in node order: the
        // lanes the reference needs that only the GPU has (those of the bodies
        // the CPU runs; every body's when verifying), and the GPU's jobs (each
        // node's bodies it runs, their sizes, and the lanes it must be sent:
        // those of their warps the host holds the newer copy of).
        warp_node_nfetch_.resize(nodes_);
        warp_node_nsend_.resize(nodes_);
        warp_node_nrec_.resize(nodes_);
        pool.run(nodes_, &World::warp_list_count_task, this);
        warp_node_ofetch_.resize(nodes_);
        warp_node_osend_.resize(nodes_);
        warp_node_orec_.resize(nodes_);
        u32 nfetch = 0, nsend = 0, nrec = 0;
        for (u32 n = 0; n < nodes_; ++n) {
            warp_node_ofetch_[n] = nfetch;
            warp_node_osend_[n] = nsend;
            warp_node_orec_[n] = nrec;
            nfetch += warp_node_nfetch_[n];
            nsend += warp_node_nsend_[n];
            nrec += warp_node_nrec_[n];
        }
        Array<WarpFetch>& fl = warp_fetch_;
        Array<u32>& fn = warp_fetch_node_;
        fl.resize(nfetch);
        fn.resize(nfetch);
        warp_send_.resize(nsend);
        warp_job_recs_.resize(nrec);
        warp_job_words_.resize(nrec);
        warp_job_chain_.resize(nrec);
        pool.run(nodes_, &World::warp_list_fill_task, this);
        warp_jobs_.clear();
        warp_job_node_.clear();
        warp_vstate_.clear();
        for (u32 n = 0; n < nodes_; ++n) {
            if (!warp_active_[n] || !warp_on_gpu_[n])
                continue;
            Soup& s = soups_[n];
            if (warp_verifying_ && warp_mismatches_ < 3) { // (for warp.verify's account of a difference)
                const u32 L = s.lanes_per_warp();
                const u32* I = s.warp_image_view().data();
                for (u32 k = 0; k < warp_node_nrec_[n]; ++k) {
                    const u32 rec = warp_job_recs_[warp_node_orec_[n] + k];
                    for (u32 i = 0; i < I[rec + WB_NWARP]; ++i) {
                        const u32 w = I[rec + WB_WARPS + i * WW_SIZE + WW_LANE0] / L;
                        warp_vstate_.push(static_cast<u64>(n) << 40 | static_cast<u64>(w) << 8 | s.lane_state(w));
                    }
                }
            }
            WarpJob j = {};
            j.node = n;
            j.img = s.warp_image().data();
            j.img_words = static_cast<u32>(s.warp_image().size());
            j.lanes = s.lanes_mut().data();
            j.nlanes = static_cast<u32>(s.lanes_mut().size());
            j.send = warp_send_.data() + warp_node_osend_[n];
            j.nsend = warp_node_nsend_[n];
            j.recs = warp_job_recs_.data() + warp_node_orec_[n];
            j.rec_words = warp_job_words_.data() + warp_node_orec_[n];
            j.rec_chain = warp_job_chain_.data() + warp_node_orec_[n];
            j.nrec = warp_node_nrec_[n];
            warp_jobs_.push(j);
            warp_job_node_.push(n);
        }
        const double tf0 = prof_ ? clock_seconds() : 0;
        if (!warp_fetch(fl, fn))
            return;
        if (prof_ && r == 0)
            prof_->w_fetch += clock_seconds() - tf0;
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
        if (dump_now) {
            if (warp_write_dump(*cst))
                fprintf(stderr, "nova: wrote the warp round of tick %llu (%u jobs) to %s\n",
                        static_cast<unsigned long long>(tick_ + 1), static_cast<u32>(warp_jobs_.size()),
                        warp_dump_path_);
            else
                fprintf(stderr, "nova: could not write the warp round to %s\n", warp_dump_path_);
            warp_dump_path_ = nullptr;
        }
        wmark(r == 0 ? &StepProfile::w_split : &StepProfile::w_later);
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
        if (ref_recs && !warp_verifying_) { // record by record, the longest bodies first
            warp_ref_build();
            const double tr = clock_seconds();
            pool.run(static_cast<u32>(warp_ref_list_.size()), &World::warp_ref_rec_task, this);
            if (!warp_jobs_.empty())
                warp_ref_wall_ += clock_seconds() - tr;
            if (prof_)
                for (usize i = 0; i < warp_ref_time_.size(); ++i)
                    prof_->n_ref += warp_ref_time_[i];
        }
        if (!warp_jobs_.empty() && !warp_gpu_.finish(&why, &pool)) {
            warp_fail(why.view());
            return;
        }
        const double round_s = clock_seconds() - t; // the GPU and the CPU at once
        gpu_s += round_s;
        t = clock_seconds();
        if (!warp_jobs_.empty())
            ++warp_gpu_rounds_;
        if (ref_recs)
            ++warp_ref_rounds_;
        warp_gpu_bodies_ += gpu_recs;
        warp_ref_bodies_ += ref_recs;
        if (warp_auto_ && !warp_verifying_)
            warp_model_measure(r, ref_recs > 0);
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
            // The first few differing rounds are described word by word (at
            // most 24 lines each): where, and the GPU's value against the reference's.
            const bool tell = warp_mismatches_ < 3;
            u32 told = 0;
            auto say = [&](const char* what, u32 node, u64 a, u64 b, u32 gpu_v, u32 ref_v) {
                if (!tell || told >= 24)
                    return;
                ++told;
                char line[200];
                snprintf(line, sizeof line, "  tick %llu round %u node %u: %s %llu.%llu: GPU %u (0x%x), reference %u (0x%x)\n",
                         static_cast<unsigned long long>(tick_), warp_round_, node, what,
                         static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), gpu_v, gpu_v, ref_v,
                         ref_v);
                warp_notes_.append(line);
            };
            for (usize i = 0; i < warp_jobs_.size(); ++i) {
                const WarpJob& j = warp_jobs_[i];
                const u32* g = gpu_img.data() + io;
                bool same_img = true; // the GPU's records (only those came back from it)
                for (u32 k = 0; k < j.nrec && same_img; ++k)
                    same_img = memcmp(g + j.recs[k], j.img + j.recs[k], static_cast<usize>(warp_job_rec_words(j, k)) * 4) == 0;
                if (!same_img) {
                    equal = false;
                    for (u32 k = 0; k < j.nrec; ++k) {
                        const u32 r0 = j.recs[k], r1 = r0 + warp_job_rec_words(j, k);
                        const u32 nw = j.img[r0 + WB_NWARP];
                        bool first = true;
                        for (u32 x = r0; x < r1; ++x) {
                            if (g[x] == j.img[x])
                                continue;
                            const u32 off = x - r0;
                            if (first) { // which body: its span, and its warps' first lanes
                                first = false;
                                say("record.span lo/hi", warp_job_node_[i], k, j.img[r0 + WB_LO], j.img[r0 + WB_HI],
                                    j.img[r0 + WB_HI]);
                                for (u32 q = 0; q < nw; ++q)
                                    say("record.warp lane0", warp_job_node_[i], k, q,
                                        j.img[r0 + WB_WARPS + q * WW_SIZE + WW_LANE0],
                                        j.img[r0 + WB_WARPS + q * WW_SIZE + WW_LANE0]);
                            }
                            if (off >= WB_WARPS && off < WB_WARPS + nw * WW_SIZE)
                                say("record.warp*100+field", warp_job_node_[i], k,
                                    ((off - WB_WARPS) / WW_SIZE) * 100 + (off - WB_WARPS) % WW_SIZE, g[x], j.img[x]);
                            else
                                say("record.offset", warp_job_node_[i], k, off, g[x], j.img[x]);
                        }
                    }
                }
                io += j.img_words;
            }
            u32 tr_node = kNone, tr_slot = 0, tr_lane = 0; // the first lane that differs
            for (usize i = 0; i < all.size(); ++i) { // the round's lanes, the GPU's against the reference's
                const u32 L = soups_[all_node[i]].lanes_per_warp();
                const Lane* ref = soups_[all_node[i]].lanes_mut().data() + static_cast<usize>(all[i].slot) * L;
                if (memcmp(all[i].dst, ref, L * sizeof(Lane)) != 0) {
                    equal = false;
                    const u32* gw = static_cast<const u32*>(all[i].dst);
                    const u32* rw = reinterpret_cast<const u32*>(ref);
                    const u32 per = sizeof(Lane) / 4;
                    for (u32 q = 0; q < L * per; ++q) {
                        if (gw[q] == rw[q])
                            continue;
                        say("warp.lane*100+word", all_node[i], all[i].slot, (q / per) * 100 + q % per, gw[q], rw[q]);
                        if (tr_node == kNone) {
                            tr_node = all_node[i];
                            tr_slot = all[i].slot;
                            tr_lane = q / per;
                        }
                    }
                }
            }
            // ... and that lane's instructions on the reference, from the
            // round's inputs again (the same result comes back).
            for (usize i = 0; tell && tr_node != kNone && i < warp_jobs_.size(); ++i) {
                if (warp_job_node_[i] != tr_node)
                    continue;
                const WarpJob& j = warp_jobs_[i];
                const u32 L = soups_[tr_node].lanes_per_warp();
                memcpy(j.img, warp_save_img_.data() + warp_save_off_[2 * i], static_cast<usize>(j.img_words) * 4);
                memcpy(soups_[tr_node].lanes_mut().data(), warp_save_lanes_.data() + warp_save_off_[2 * i + 1],
                       static_cast<usize>(j.nlanes) * sizeof(Lane));
                const Lane& s0 = soups_[tr_node].lanes_mut()[static_cast<usize>(tr_slot) * L + tr_lane];
                int st = -1;
                for (usize q = 0; q < warp_vstate_.size(); ++q)
                    if ((warp_vstate_[q] >> 8) == (static_cast<u64>(tr_node) << 32 | tr_slot))
                        st = static_cast<int>(warp_vstate_[q] & 0xFF);
                bool sent = false;
                for (u32 q = 0; q < j.nsend; ++q)
                    sent = sent || j.send[q] == tr_slot;
                char line[320];
                snprintf(line, sizeof line,
                         "  the reference, warp %u lane %u: ip %u ax %08x bx %08x cx %08x dx %08x sp %u at the start; "
                         "its lanes were %s before the round (state %d) and %s\n",
                         tr_slot, tr_lane, s0.ip, s0.ax, s0.bx, s0.cx, s0.dx, s0.sp,
                         st == Soup::kLanesHost ? "the host's" : st == Soup::kLanesDevice ? "the GPU's"
                                                             : st == Soup::kLanesBoth   ? "on both" : "?",
                         st, sent ? "were sent to the GPU" : "were not sent");
                warp_notes_.append(line);
                for (u32 q = 0; q < L; ++q) { // every lane of the warp at the start
                    const Lane& x = soups_[tr_node].lanes_mut()[static_cast<usize>(tr_slot) * L + q];
                    snprintf(line, sizeof line,
                             "    lane %2u: ip %u ax %08x bx %08x cx %08x dx %08x sp %u halted %u yielded %u errs %u\n", q,
                             x.ip, x.ax, x.bx, x.cx, x.dx, x.sp, x.halted, x.yielded, x.errs);
                    warp_notes_.append(line);
                }
                warp_trace_begin(tr_slot * L, tr_lane, &warp_notes_);
                soups_[tr_node].warp_run_ref();
                warp_trace_end();
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
        tw = prof_ ? clock_seconds() : 0;
        pool.run(nodes_, &World::warp_unpack_task, this);
        wmark(r == 0 ? &StepProfile::w_unpack : &StepProfile::w_later);
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
        if (!fl.empty()) { // most came back with the round (gathered on the GPU): fetch only the rest
            warp_got_.resize(fl.size());
            if (warp_gpu_.take_gathered(fl.data(), static_cast<u32>(fl.size()), warp_got_.data(), &pool) > 0) {
                usize k = 0;
                for (usize i = 0; i < fl.size(); ++i) {
                    if (warp_got_[i]) {
                        soups_[fn[i]].set_lane_state(fl[i].slot, Soup::kLanesBoth);
                    } else {
                        fl[k] = fl[i];
                        fn[k] = fn[i];
                        ++k;
                    }
                }
                fl.resize(k);
                fn.resize(k);
            }
        }
        if (!warp_fetch(fl, fn))
            return;
        wmark(r == 0 ? &StepProfile::w_hostlanes : &StepProfile::w_later);
        pool.run(nodes_, &World::warp_host_task, this);
        wmark(r == 0 ? &StepProfile::w_host : &StepProfile::w_later);
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
        // Only a tick's first round counts, against the tick before's: later
        // rounds run the few warps a host instruction freed, at a cost a step
        // nothing like the first's. And the first round after a tick without
        // the GPU (or a start) is left out altogether: every lane crosses over
        // in it, so it reads dear and the next would read cheap, which kept a
        // search going whichever way it was heading (Phase 10 profiling: down
        // to 0.1% of the nodes on the GPU, at four times the cost).
        const bool modelled = warp_gstep_ > 0 && warp_hstep_ > 0; // (then the model sets the limit instead)
        if (use_gpu && warp_auto_ && !warp_verifying_ && !modelled && r == 0 && steps_after > steps_before) {
            const double cost = round_s / static_cast<double>(steps_after - steps_before);
            if (warp_climb_fresh_) {
                warp_climb_fresh_ = false;
                warp_cost_ = 0;
            } else {
                const bool follows = warp_cost_ > 0 && warp_cost_tick_ + 1 == tick_;
                if (follows && cost > warp_cost_ * 1.02) {
                    warp_dir_ = -warp_dir_;
                    warp_stride_ = warp_stride_ * 0.5 < 0.125 ? 0.125 : warp_stride_ * 0.5;
                } else if (follows && cost < warp_cost_) {
                    warp_stride_ = warp_stride_ * 1.25 > 2.0 ? 2.0 : warp_stride_ * 1.25;
                }
                warp_cost_ = cost;
                warp_cost_tick_ = tick_;
                const double top = 2.0 * (static_cast<double>(longest_all) + 1.0);
                double lim =
                    (warp_limit_ < 1 ? 1.0 : static_cast<double>(warp_limit_)) * pow(2.0, warp_dir_ * warp_stride_);
                lim = lim > top ? top : lim;
                warp_limit_ = lim > 1e9 ? 1000000000u : static_cast<u32>(lim);
            }
        }
    }
    tw = prof_ ? clock_seconds() : 0;
    pool.run(nodes_, &World::warp_second_task, this);
    wmark(&StepProfile::w_second);
    if (prof_) { // the slowest node in each pool phase: the floor under its wall time
        double m[3] = {0, 0, 0};
        for (u32 n = 0; n < nodes_; ++n)
            for (u32 k = 0; k < 3; ++k)
                m[k] = warp_node_t_[3 * n + k] > m[k] ? warp_node_t_[3 * n + k] : m[k];
        prof_->w_first_max += m[0];
        prof_->w_pack_max += m[1];
        prof_->w_second_max += m[2];
    }
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
            in_flight_[c.edge] -= c.dormant ? 0 : 1;
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
            c.dormant = dep.dormant;
            c.mem_reserve = dep.mem_reserve;
            c.hops = dep.hops;
            c.ticks = static_cast<u32>(c.due - tick_);
            cargo_codons_.append(s.departure_codons() + dep.codon_off, dep.len);
            cargo_.push(c);
            in_flight_[e] += c.dormant ? 0 : 1; // (dormant programs travel outside the capacity)
        }
        s.clear_departures();
    }
}

// Dormancy and drift (docs/dormancy-plan.md): a random open path out of node
// n (not down, not to a fallow site) from a stream keyed by the cargo's send
// number and hop, so the choice doesn't depend on delivery order; kNone if
// there is none.
u32 World::drift_edge(u32 n, u64 seq, u32 hops) const {
    const u32 deg = out_begin_[n + 1] - out_begin_[n];
    u32 open = 0;
    for (u32 k = 0; k < deg; ++k)
        open += drift_open(port_list_[out_begin_[n] + k]) ? 1 : 0;
    if (open == 0)
        return kNone;
    Rng r(seed_, Rng::kWorldNode, 0x6472696674000000ull + (seq << 8) + hops); // "drift"
    u32 pick = static_cast<u32>(r.below(open));
    for (u32 k = 0; k < deg; ++k) {
        const u32 e = port_list_[out_begin_[n] + k];
        if (drift_open(e) && pick-- == 0)
            return e;
    }
    return kNone;
}

bool World::drift_open(u32 e) const {
    if (lifecycle_ && !live_[edge_dst_[e]])
        return false;
    if (!port_down_.empty() && port_down_[e])
        return false;
    if (!link_down_.empty() && link_down_[e])
        return false;
    return true;
}

// A drifting dormant program due here: the clock first (its reserves spent
// over the ticks it travelled; spent out, it dies on the way), then it lands
// if the node has room for its code, else it passes through to a random open
// path out of here, a hop with no rest (dying with nowhere to go). Passers-by
// are put on their paths after this barrier's deliveries (drift_out_).
void World::deliver_dormant(Cargo& c) {
    const u32 dst = edge_dst_[c.edge];
    const SoupParams& np = node_params_[dst];
    u64 spend = static_cast<u64>(np.dormant_mem_loss) * c.ticks;
    if (spend <= c.mem_reserve) {
        c.mem_reserve -= static_cast<u32>(spend);
    } else {
        const u64 over = np.dormant_mem_loss ? (spend - c.mem_reserve) / np.dormant_mem_loss : 0; // ticks on disk
        c.mem_reserve = 0;
        const u64 dspend = static_cast<u64>(np.dormant_disk_loss) * over;
        c.disk = dspend < c.disk ? static_cast<u32>(c.disk - dspend) : 0;
    }
    if (c.mem_reserve == 0 && c.disk == 0) {
        ++world_dormant_expired_;
        return;
    }
    Dormant d = {};
    d.mem_reserve = c.mem_reserve;
    d.disk_reserve = c.disk;
    d.millicores = c.millicores;
    d.emillicores = c.emillicores;
    d.gpus = c.gpus;
    d.npus = c.npus;
    d.form = c.form;
    d.kind = c.kind;
    d.hops = c.hops;
    if (live_.empty() || live_[dst])
        if (soups_[dst].land_dormant(cargo_codons_.data() + c.codon_off, c.len, d, c.tag))
            return;
    const u32 e = drift_edge(dst, c.seq, c.hops);
    if (e == kNone) {
        ++world_dormant_expired_;
        return;
    }
    Cargo n = c;
    const u32 bw = edge_bandwidth_[e];
    n.edge = e;
    n.due = tick_ + (c.len + bw - 1) / bw + edge_latency_[e]; // as if it left at this barrier
    n.ticks = static_cast<u32>(n.due - tick_);
    n.seq = next_seq_++; // (a fresh send number: its corruption stream is its own)
    n.hops = c.hops + 1;
    n.codon_off = static_cast<u32>(drift_codons_.size());
    drift_codons_.append(cargo_codons_.data() + c.codon_off, c.len);
    drift_out_.push(n);
    ++world_dormant_hops_;
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
    drift_out_.clear();
    drift_codons_.clear();
    for (usize i = 0; i < order_scratch_.size(); ++i) {
        Cargo& c = cargo_[due_scratch_[order_scratch_[i] & 0xFFFFFF]];
        if (edge_corruption_ > 0)
            corrupt(c);
        if (c.dormant) { // a drifting dormant program (outside the capacity)
            deliver_dormant(c);
            continue;
        }
        soups_[edge_dst_[c.edge]].arrive(cargo_codons_.data() + c.codon_off, c.len, c.millicores, c.gpus,
                                         c.npus, c.disk, c.tag, c.form, c.emillicores, static_cast<u8>(c.kind));
        --in_flight_[c.edge];
    }
    // Keep the rest, in order, with their codons packed; then the dormant
    // programs passing through, in the order they arrived.
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
    for (usize i = 0; i < drift_out_.size(); ++i) {
        Cargo c = drift_out_[i];
        const u32 off = static_cast<u32>(codons.size());
        codons.append(drift_codons_.data() + c.codon_off, c.len);
        c.codon_off = off;
        cargo_.push(c);
    }
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

void World::set_profile(StepProfile* p) {
    prof_ = p;
    if (model_ == Model::soup)
        for (u32 n = 0; n < nodes_; ++n)
            soups_[n].set_timing(p != nullptr);
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
            // Every node on the reference, in the same phases as with the GPU
            // (the same world), so a heavy node's warps spread over the threads.
            warp_step(pool, false);
            ++warp_plain_ticks_;
            warp_climb_fresh_ = true; // the next GPU round sends every lane: the split's search skips it
        } else {
            warp_step(pool);
        }
        if (warp_auto_) { // a mode's recent time: not the switch's tick, then fresh, then blended
            const u32 m = plain ? 1 : 0;
            const double s = clock_seconds() - t0;
            warp_run_len_ = warp_last_plain_ == m ? warp_run_len_ + 1 : 1;
            if (warp_run_len_ == 2)
                warp_tick_s_[m] = s;
            else if (warp_run_len_ > 2)
                warp_tick_s_[m] = warp_tick_s_[m] * 0.875 + s * 0.125;
            warp_last_plain_ = m;
        }
    } else if (model_ == Model::soup && soup_params_.warps) {
        warp_step(pool, false); // warps on the reference alone, record by record over the threads
    } else {
        pool.run(nodes_, &World::update_task, this);
    }
    if (warp_failed_)
        return;
    if (prof_) {
        ++prof_->ticks;
        lap(&prof_->update);
        if (model_ == Model::soup) { // each node's parts (thread-seconds)
            for (u32 n = 0; n < nodes_; ++n) {
                Soup::Timing& st = soups_[n].timing();
                prof_->n_first += st.first;
                prof_->n_bodies += st.bodies;
                prof_->n_pack += st.pack;
                prof_->n_ref += st.ref;
                prof_->n_unpack += st.unpack;
                prof_->n_host += st.host;
                prof_->n_second += st.second;
                st = Soup::Timing{};
            }
            t = clock_seconds(); // (summing isn't the barrier's time)
        }
    }
    if (model_ == Model::soup) {
        // Dispersal (Phase 3): departures onto edges, then arrivals due now.
        // Then sum the tallies (arrivals count on their destination), in node
        // order.
        world_corrupted_ = world_transit_lost_ = 0;
        world_dormant_expired_ = world_dormant_hops_ = 0;
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
        soup_last_.dormant_expired += world_dormant_expired_; // (those that died on the way)
        soup_last_.dormant_hops += world_dormant_hops_;       // (those that passed through)
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
                if (soup_params_.dormancy) { // a drifting dormant program, its memory reserve and hops
                    h.word(c.dormant);
                    h.word((static_cast<u64>(c.mem_reserve) << 32) | c.hops);
                }
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
                if (soup_params_.dormancy) { // dormancy (v3)
                    w.put_u32(c.dormant);
                    w.put_u32(c.mem_reserve);
                    w.put_u32(c.hops);
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

bool World::load_state(ByteReader& r, TextBuf* err, u32 version) {
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
            if (!soups_[n].load(r, err, version))
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
                if (soup_params_.dormancy && version >= 3) { // (none before v3)
                    r.get_u32(&c.dormant);
                    r.get_u32(&c.mem_reserve);
                    r.get_u32(&c.hops);
                }
                if (!r.ok() || c.edge >= edge_src_.size() || c.len == 0 || c.len > r.remaining() ||
                    c.due < tick || c.seq >= next_seq_ || c.form > kFormGpu || c.kind > kKindE || c.dormant > 1) {
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
