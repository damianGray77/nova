#include "nova/world.h"

#include <cstdio> // snprintf

#include "nova/hash.h"
#include "nova/rng.h"

namespace nova {

namespace {

// The live knobs. Everything else World::init reads is structural.
enum class Knob { none, send_rate, sends_per_tick, mutations_per_tick, max_parcel };

struct KnobValue {
    Knob knob = Knob::none;
    double f = 0;
    u32 u = 0;
};

Knob knob_of(Str key) {
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
    return key.eq("seed") || key.starts_with("world.");
}

bool parse_u32_in(Str v, u32 lo, u32 hi, u32* out) {
    u64 x;
    if (!parse_u64(v, &x) || x < lo || x > hi)
        return false;
    *out = static_cast<u32>(x);
    return true;
}

KnobResult parse_knob(Str key, Str v, KnobValue* out) {
    out->knob = knob_of(key);
    switch (out->knob) {
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
    u32 chords = get_u32_in(cfg, "world.chords", nodes_ / 2, 0, 10000000);
    soup_size_ = get_u32_in(cfg, "world.soup_size", 256, 1, 1u << 24);
    pool_capacity_ = get_u32_in(cfg, "world.pool_capacity", 1000, 1, 1000000000);
    u32 pool_initial = get_u32_in(cfg, "world.pool_initial", pool_capacity_ / 2, 0, pool_capacity_);
    outbox_capacity_ = get_u32_in(cfg, "world.outbox_capacity", 2, 1, 1024);

    for (usize i = 0; i < sizeof kKnobKeys / sizeof kKnobKeys[0]; ++i) {
        Str key = kKnobKeys[i];
        Str v = cfg.get_str(key, kKnobDefaults[i]);
        KnobResult r = set_knob(key, v);
        if (r != KnobResult::ok)
            cfg.fail(key, describe(r));
    }
    if (!cfg.ok())
        return false;

    // --- graph: a ring plus random chords, from the world-level stream ----
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

    // --- node state ---------------------------------------------------------
    tick_ = 0;
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

KnobResult World::check_knob(Str key, Str value) const {
    KnobValue kv;
    return parse_knob(key, value, &kv);
}

KnobResult World::set_knob(Str key, Str value) {
    KnobValue kv;
    KnobResult r = parse_knob(key, value, &kv);
    if (r != KnobResult::ok)
        return r;
    switch (kv.knob) {
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
    case Knob::none:
        break;
    }
    return KnobResult::ok;
}

void World::update_task(void* ctx, u32 node) {
    static_cast<World*>(ctx)->update_node(node);
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
    pool.run(nodes_, &World::update_task, this);
    deliver();
    ++tick_;
}

u64 World::total_units() const {
    u64 sum = 0;
    for (u32 n = 0; n < nodes_; ++n)
        sum += pool_[n];
    return sum;
}

u64 World::hash() const {
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
    w.put_u64(tick_);
    w.put_u32(nodes_);
    w.put_u32(soup_size_);
    w.put_u32(edge_count());
    for (usize e = 0; e < edge_src_.size(); ++e) {
        w.put_u32(edge_src_[e]);
        w.put_u32(edge_dst_[e]);
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
