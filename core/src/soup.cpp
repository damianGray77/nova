#include "nova/soup.h"

#include <intrin.h> // _BitScanForward64, _BitScanReverse64

#include <cmath>  // floor, log
#include <cstdio> // snprintf

namespace nova {

// ---------------------------------------------------------------------------
// Heat curves
// ---------------------------------------------------------------------------

float HeatCurve::speed(float h) const {
    if (h <= hot)
        return 1.0f;
    if (h >= max)
        return speed_min;
    return 1.0f - (1.0f - speed_min) * (h - hot) / (max - hot);
}

float HeatCurve::err_mult(float h) const {
    if (h <= warm)
        return 1.0f;
    if (h >= max)
        return err_max;
    return 1.0f + (err_max - 1.0f) * (h - warm) / (max - warm);
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

namespace {

u32 get_u32(Config& cfg, Str key, u64 def, u64 lo, u64 hi) {
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

double get_f64(Config& cfg, Str key, double def, double lo, double hi) {
    double v = cfg.get_f64(key, def);
    if (!(v >= lo && v <= hi)) {
        char what[96];
        snprintf(what, sizeof what, "must be between %g and %g", lo, hi);
        cfg.fail(key, what);
        return lo;
    }
    return v;
}

HeatCurve get_curve(Config& cfg, const char* prefix, HeatCurve def) {
    char key[64];
    HeatCurve c = {};
    snprintf(key, sizeof key, "%s.warm", prefix);
    c.warm = static_cast<float>(get_f64(cfg, key, def.warm, 0, 1e9));
    snprintf(key, sizeof key, "%s.hot", prefix);
    c.hot = static_cast<float>(get_f64(cfg, key, def.hot, 0, 1e9));
    snprintf(key, sizeof key, "%s.max", prefix);
    c.max = static_cast<float>(get_f64(cfg, key, def.max, 0, 1e9));
    snprintf(key, sizeof key, "%s.speed_min", prefix);
    c.speed_min = static_cast<float>(get_f64(cfg, key, def.speed_min, 0.001, 1));
    snprintf(key, sizeof key, "%s.err_max", prefix);
    c.err_max = static_cast<float>(get_f64(cfg, key, def.err_max, 1, 1e6));
    if (!(c.warm < c.hot && c.hot < c.max)) {
        snprintf(key, sizeof key, "%s.warm", prefix);
        cfg.fail(key, "onsets must be staggered: warm < hot < max");
    }
    return c;
}

} // namespace

bool genome_from_names(const CodonTable& t, Str names, Array<u8>* out, TextBuf* err) {
    out->clear();
    usize i = 0;
    while (i < names.n) {
        while (i < names.n && (names.p[i] == ' ' || names.p[i] == '\t'))
            ++i;
        if (i == names.n)
            break;
        usize start = i;
        while (i < names.n && names.p[i] != ' ' && names.p[i] != '\t')
            ++i;
        Str word = names.sub(start, i - start);
        Op op;
        if (!op_from_name(word, &op)) {
            err->append("unknown instruction '");
            err->append(word);
            err->append_char('\'');
            return false;
        }
        int c = codon_of(t, op);
        if (c < 0) {
            err->append("instruction '");
            err->append(word);
            err->append("' is not in the codon table");
            return false;
        }
        out->push(static_cast<u8>(c));
    }
    return true;
}

bool soup_params_from_config(Config& cfg, u32 soup_size, SoupParams* p) {
    p->size = soup_size;
    p->millicores = get_u32(cfg, "world.millicores", 4000, 1, 10000000);
    p->instr_per_core = get_u32(cfg, "cpu.instr_per_core", 1000, 1, 100000000);
    p->halt_after = get_u32(cfg, "cpu.halt_after", 8, 1, 255);

    Str table = cfg.get_str("isa.table", "nova_v1");
    if (table.eq("custom")) {
        Str codons = cfg.get_str("isa.codons", "");
        TextBuf err;
        if (!parse_table(codons, &p->table, &err))
            cfg.fail("isa.codons", err.c_str());
    } else if (!builtin_table(table, &p->table)) {
        cfg.fail("isa.table", "must be nova_v1, tierra0 or custom");
    }
    Str membrane = cfg.get_str("isa.membrane", "private");
    if (membrane.eq("private"))
        p->open_membrane = false;
    else if (membrane.eq("open"))
        p->open_membrane = true;
    else
        cfg.fail("isa.membrane", "must be private or open");
    p->stack_depth = get_u32(cfg, "isa.stack_depth", 8, 1, kMaxStack);
    p->search_limit = get_u32(cfg, "isa.search_limit", 400, 1, soup_size);
    p->max_template = get_u32(cfg, "isa.max_template", 8, 1, 64);
    p->tierra_share = get_u32(cfg, "tierra.divide_share", 32, 0, 64);

    p->spark_rate = get_f64(cfg, "spark.rate", 0.0, 0.0, 1e6);
    p->spark_initial = get_u32(cfg, "spark.initial", 0, 0, 10000000);
    p->spark_grant = get_u32(cfg, "spark.grant", 10, 1, 10000000);
    p->spark_min = get_u32(cfg, "spark.min_len", 8, 1, soup_size);
    p->spark_max = get_u32(cfg, "spark.max_len", 128 < soup_size ? 128 : soup_size, 1, soup_size);
    if (p->spark_min > p->spark_max)
        cfg.fail("spark.min_len", "must not exceed spark.max_len");

    p->heat_on = cfg.get_bool("heat.enabled", true);
    p->cool = static_cast<float>(get_f64(cfg, "heat.cool", 0.1, 0.0001, 1.0));
    p->cpu_gain = static_cast<float>(get_f64(cfg, "heat.cpu_gain", 6.0, 0, 1e9));
    p->fetch_gain = static_cast<float>(get_f64(cfg, "heat.fetch_gain", 0.001, 0, 1e9));
    p->write_gain = static_cast<float>(get_f64(cfg, "heat.write_gain", 0.01, 0, 1e9));
    p->cpu = get_curve(cfg, "heat.cpu", HeatCurve{40, 60, 100, 0.25f, 10, 0});
    p->mem = get_curve(cfg, "heat.mem", HeatCurve{40, 70, 100, 0.25f, 10, 0});

    // Ray's baselines (design.md 6.3): about 1 copy error per 2,000 codons
    // copied; about 1 background flip per 10,000 instructions executed, which
    // at full load is 1e-4 * millicores * instr_per_core / 1000 flips per tick,
    // spread over the soup.
    double nominal = static_cast<double>(p->millicores) * p->instr_per_core / 1000.0;
    p->copy_rate = get_f64(cfg, "hazard.copy", 5e-4, 0, 1);
    p->flaw_rate = get_f64(cfg, "hazard.flaw", 1e-4, 0, 1);
    p->rot_rate = get_f64(cfg, "hazard.rot", 1e-4 * nominal / soup_size, 0, 1);

    p->contact_range = get_u32(cfg, "contact.range", 256, 0, soup_size);
    p->contact_base = get_u32(cfg, "contact.cost_base", 8, 0, 1000000);
    p->contact_per_codon = get_f64(cfg, "contact.cost_per_codon", 1.0, 0, 1e6);

    Str genome = cfg.get_str("seed.genome", "");
    p->seed_at = get_u32(cfg, "seed.at", 0, 0, soup_size - 1);
    p->seed_millicores = get_u32(cfg, "seed.millicores", 1000, 0, 10000000);
    if (cfg.ok() && !genome.empty()) {
        TextBuf err;
        if (!genome_from_names(p->table, genome, &p->seed_genome, &err))
            cfg.fail("seed.genome", err.c_str());
        else if (p->seed_genome.size() > soup_size - p->seed_at)
            cfg.fail("seed.genome", "does not fit in the soup at seed.at");
    }
    return cfg.ok();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void Soup::init(const SoupParams* p, u64 seed, u32 node) {
    p_ = p;
    seed_ = seed;
    node_ = node;
    rng_ = Rng(seed, node, Rng::kInitTick);
    const u32 n = p->size;

    // Everything starts as uniformly random waste (6.3: codon 0 is NOP0, so a
    // zeroed soup would be a sea of labels).
    mem_.resize(n);
    for (u32 i = 0; i < n; ++i)
        mem_[i] = static_cast<u8>(rng_.next() & 63);
    owner_.resize(n);
    owner_.fill(kNone);
    used_.resize((static_cast<usize>(n) + 63) / 64);
    used_.fill(0);
    waste_ = n;

    blocks_.clear();
    free_blocks_.clear();
    bodies_.clear();
    free_bodies_.clear();
    cpus_.clear();
    free_cpus_.clear();
    order_.clear();
    next_serial_ = 0;

    const u32 m = p->millicores;
    unit_owner_.resize(m);
    unit_owner_.fill(kNone);
    unit_heat_.resize(m);
    unit_heat_.fill(0.0f);
    unit_next_.resize(m);
    unit_next_.fill(kNone);
    unit_prev_.resize(m);
    unit_prev_.fill(kNone);
    pool_.resize(m);
    pool_pos_.resize(m);
    for (u32 u = 0; u < m; ++u) {
        pool_[u] = u;
        pool_pos_[u] = u;
    }

    const u32 pages = (n + kPage - 1) >> kPageBits;
    page_heat_.resize(pages);
    page_heat_.fill(0.0f);
    page_fetch_.resize(pages);
    page_fetch_.fill(0);
    page_write_.resize(pages);
    page_write_.fill(0);

    tally_ = {};
    if (!p->seed_genome.empty()) {
        u32 len = static_cast<u32>(p->seed_genome.size());
        for (u32 i = 0; i < len; ++i)
            mem_[p->seed_at + i] = p->seed_genome[i];
        create_body(p->seed_at, len, p->seed_millicores);
    }
    sparks(p->spark_initial);
}

// ---------------------------------------------------------------------------
// Ownership
// ---------------------------------------------------------------------------

void Soup::mark_used(u32 start, u32 len, bool used) {
    for (u32 a = start; a < start + len; ++a) {
        u64 bit = 1ull << (a & 63);
        if (used)
            used_[a >> 6] |= bit;
        else
            used_[a >> 6] &= ~bit;
    }
}

bool Soup::is_free(u32 start, u32 len) const {
    if (len == 0 || start >= p_->size || len > p_->size - start)
        return false;
    for (u32 a = start; a < start + len; ++a)
        if (owner_[a] != kNone)
            return false;
    return true;
}

// First p >= from with [p, p + len) all waste (blocks never wrap the soup end).
// Works a word at a time: bit scans jump over whole free and used runs, so a
// fragmented region costs a few steps per word, not one per codon.
u32 Soup::find_free_fwd(u32 from, u32 len) const {
    const u32 n = p_->size;
    if (len == 0 || len > n)
        return kNone;
    u32 run = 0; // free codons ending just before a
    u32 a = from;
    while (a < n) {
        const u32 bit = a & 63;
        const u32 avail = min_of<u32>(64 - bit, n - a); // codons from a in this word
        const u64 w = used_[a >> 6] >> bit;             // bit 0 is codon a
        unsigned long tz;
        u32 free_here = _BitScanForward64(&tz, w) ? min_of<u32>(tz, avail) : avail;
        run += free_here;
        if (run >= len)
            return a + free_here - run;
        if (free_here == avail) {
            a += avail;
            continue;
        }
        // Codon a + tz is owned: skip the owned run that starts there.
        unsigned long ones;
        u32 used = _BitScanForward64(&ones, ~(w >> tz)) ? ones : 64 - tz;
        a += tz + used;
        run = 0;
    }
    return kNone;
}

// Largest p with p + len <= to and [p, p + len) all waste (scanning down).
u32 Soup::find_free_bwd(u32 to, u32 len) const {
    if (len == 0 || len > to)
        return kNone;
    u32 run = 0, top = 0; // free codons from top down to just above a
    i64 a = static_cast<i64>(to) - 1;
    while (a >= 0) {
        const u32 ua = static_cast<u32>(a);
        const u32 bit = ua & 63;
        const u32 avail = bit + 1;                    // codons ua down to the word's start
        const u64 w = used_[ua >> 6] << (63 - bit); // bit 63 is codon ua
        unsigned long hi;
        u32 free_here = _BitScanReverse64(&hi, w) ? 63 - hi : avail;
        if (free_here > 0) {
            if (run == 0)
                top = ua;
            run += free_here;
            if (run >= len)
                return top + 1 - len;
        }
        if (free_here == avail) {
            a -= avail;
            continue;
        }
        // Codon ua - free_here is owned: skip the owned run going down.
        unsigned long z;
        u32 used = _BitScanReverse64(&z, ~(w << free_here)) ? 63 - z : avail - free_here;
        a -= static_cast<i64>(free_here) + used;
        run = 0;
    }
    return kNone;
}

u32 Soup::new_block(u32 start, u32 len, u32 body) {
    u32 k;
    if (!free_blocks_.empty()) {
        k = free_blocks_.back();
        free_blocks_.pop();
    } else {
        k = static_cast<u32>(blocks_.size());
        blocks_.push(Block{});
    }
    Block& bl = blocks_[k];
    bl.start = start;
    bl.len = len;
    bl.body = body;
    bl.next = kNone;
    bl.owned = len;
    bl.pad = 0;
    for (u32 a = start; a < start + len; ++a)
        owner_[a] = k;
    mark_used(start, len, true);
    waste_ -= len;
    // Append, so the body's first block (its surface) stays first.
    Body& b = bodies_[body];
    if (b.blocks == kNone) {
        b.blocks = k;
    } else {
        u32 t = b.blocks;
        while (blocks_[t].next != kNone)
            t = blocks_[t].next;
        blocks_[t].next = k;
    }
    b.mem += len;
    return k;
}

void Soup::free_block(u32 k) {
    ++free_epoch_;
    Block& bl = blocks_[k];
    for (u32 a = bl.start; a < bl.start + bl.len; ++a) {
        if (owner_[a] == k) {
            owner_[a] = kNone;
            used_[a >> 6] &= ~(1ull << (a & 63));
            ++waste_;
        }
    }
    bl.body = kNone;
    bl.next = kNone;
    bl.owned = 0;
    free_blocks_.push(k);
}

void Soup::forget_block(u32 body, u32 k) {
    for (u32 c = bodies_[body].cpus; c != kNone; c = cpus_[c].next)
        if (cpus_[c].block == k)
            cpus_[c].block = kNone;
}

u32 Soup::new_body() {
    u32 b;
    if (!free_bodies_.empty()) {
        b = free_bodies_.back();
        free_bodies_.pop();
    } else {
        b = static_cast<u32>(bodies_.size());
        bodies_.push(Body{});
    }
    Body& B = bodies_[b];
    B = Body{};
    B.serial = next_serial_++;
    B.blocks = kNone;
    B.latest = kNone;
    B.cpus = kNone;
    B.unit_head = kNone;
    B.unit_tail = kNone;
    B.alive = 1;
    B.err = 1.0f;
    order_.push(b);
    return b;
}

u32 Soup::new_cpu(u32 body, u32 start) {
    u32 c;
    if (!free_cpus_.empty()) {
        c = free_cpus_.back();
        free_cpus_.pop();
    } else {
        c = static_cast<u32>(cpus_.size());
        cpus_.push(Cpu{});
    }
    Cpu& C = cpus_[c];
    C = Cpu{};
    C.ip = C.rh = C.wh = start;
    C.body = body;
    C.block = owner_[start];
    C.next = kNone;
    Body& B = bodies_[body];
    if (B.cpus == kNone) {
        B.cpus = c;
    } else {
        u32 t = B.cpus;
        while (cpus_[t].next != kNone)
            t = cpus_[t].next;
        cpus_[t].next = c;
    }
    ++B.ncpu;
    return c;
}

u32 Soup::create_body(u32 start, u32 len, u32 units) {
    if (!is_free(start, len))
        return kNone;
    u32 b = new_body();
    new_block(start, len, b);
    new_cpu(b, start);
    claim_units(b, units);
    return b;
}

void Soup::kill(u32 b) {
    Body& B = bodies_[b];
    for (u32 k = B.blocks; k != kNone;) {
        u32 next = blocks_[k].next;
        free_block(k);
        k = next;
    }
    while (B.unit_head != kNone) {
        u32 u = B.unit_head;
        unlink_unit(u);
        pool_push(u);
    }
    for (u32 c = B.cpus; c != kNone;) {
        u32 next = cpus_[c].next;
        cpus_[c].body = kNone;
        free_cpus_.push(c);
        c = next;
    }
    B.blocks = B.cpus = B.latest = kNone;
    B.ncpu = 0;
    B.mem = 0;
    B.alive = 0;
    ++tally_.deaths;
}

// ---------------------------------------------------------------------------
// CPU units (the millicore ledger)
// ---------------------------------------------------------------------------

void Soup::pool_push(u32 u) {
    unit_owner_[u] = kNone;
    pool_pos_[u] = static_cast<u32>(pool_.size());
    pool_.push(u);
}

void Soup::pool_remove(u32 u) {
    u32 i = pool_pos_[u];
    u32 last = pool_.back();
    pool_[i] = last;
    pool_pos_[last] = i;
    pool_.pop();
    pool_pos_[u] = kNone;
}

void Soup::append_unit(u32 body, u32 u) {
    Body& B = bodies_[body];
    unit_owner_[u] = body;
    unit_prev_[u] = B.unit_tail;
    unit_next_[u] = kNone;
    if (B.unit_tail != kNone)
        unit_next_[B.unit_tail] = u;
    else
        B.unit_head = u;
    B.unit_tail = u;
    ++B.units;
}

void Soup::unlink_unit(u32 u) {
    Body& B = bodies_[unit_owner_[u]];
    u32 prev = unit_prev_[u], next = unit_next_[u];
    if (prev != kNone)
        unit_next_[prev] = next;
    else
        B.unit_head = next;
    if (next != kNone)
        unit_prev_[next] = prev;
    else
        B.unit_tail = prev;
    unit_prev_[u] = unit_next_[u] = kNone;
    unit_owner_[u] = kNone;
    --B.units;
}

// Claims up to k units chosen at random from the pool (the world picks no
// favourites: hot and cold free units are equally likely). Returns the count.
u32 Soup::claim_units(u32 body, u32 k) {
    if (k > pool_.size())
        k = static_cast<u32>(pool_.size());
    for (u32 i = 0; i < k; ++i) {
        u32 u = pool_[rng_.below(pool_.size())];
        pool_remove(u);
        append_unit(body, u);
    }
    return k;
}

void Soup::move_units(u32 from, u32 to, u32 k) {
    for (u32 i = 0; i < k && bodies_[from].unit_head != kNone; ++i) {
        u32 u = bodies_[from].unit_head;
        unlink_unit(u);
        append_unit(to, u);
    }
}

void Soup::release_units(u32 body, u32 k) {
    for (u32 i = 0; i < k && bodies_[body].unit_head != kNone; ++i) {
        u32 u = bodies_[body].unit_head;
        unlink_unit(u);
        pool_push(u); // freed units keep their heat (6.4 #2)
    }
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void Soup::begin_tick(u64 tick) {
    rng_ = Rng(seed_, node_, tick);
    tally_ = {};
    // Hazard countdowns in "exposure" units: each instruction (or copied codon)
    // uses up its body's error multiplier. An Exp(1)/rate draw makes the events
    // a Poisson process whose rate follows heat exactly; redrawing every tick is
    // exact too (memorylessness), so no countdown state crosses ticks.
    flaw_left_ = p_->flaw_rate > 0 ? -log(1.0 - rng_.unit()) / p_->flaw_rate : 1e300;
    copy_left_ = p_->copy_rate > 0 ? -log(1.0 - rng_.unit()) / p_->copy_rate : 1e300;
}

void Soup::tick(u64 tick) {
    begin_tick(tick);

    double rate = p_->spark_rate;
    u32 count = static_cast<u32>(rate);
    if (rng_.chance(rate - count))
        ++count;
    sparks(count);

    prepare_heat();

    // Fixed cyclic order (creation order) with a start rotated by the node RNG,
    // so no body has a permanent first-mover edge (7.1 D). Bodies born during
    // the tick are appended and first run next tick.
    const usize n = order_.size();
    if (n > 0) {
        usize start = static_cast<usize>(rng_.below(n));
        for (usize k = 0; k < n; ++k) {
            u32 b = order_[(start + k) % n];
            if (bodies_[b].alive)
                run_body(b, tick);
        }
    }

    heat_update();
    bit_rot();
    death_sweep();
}

void Soup::sparks(u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (pool_.empty())
            return; // a spark only draws from the free pool
        u32 len = p_->spark_min + static_cast<u32>(rng_.below(p_->spark_max - p_->spark_min + 1));
        u32 from = static_cast<u32>(rng_.below(p_->size));
        u32 at = known_full(len) ? kNone : find_free_fwd(from, len);
        if (at == kNone && !known_full(len))
            at = find_free_fwd(0, len);
        if (at == kNone) {
            note_full(len);
            continue;
        }
        create_body(at, len, p_->spark_grant);
        ++tally_.sparks;
    }
}

void Soup::prepare_heat() {
    for (usize k = 0; k < order_.size(); ++k) {
        Body& B = bodies_[order_[k]];
        B.speed = 0;
        B.err = 0;
        B.work = 0;
    }
    const u32 m = p_->millicores;
    for (u32 u = 0; u < m; ++u) {
        u32 o = unit_owner_[u];
        if (o == kNone)
            continue;
        Body& B = bodies_[o];
        float h = unit_heat_[u];
        B.speed += p_->heat_on ? p_->cpu.speed(h) : 1.0f;
        B.err += h;
    }
    for (usize k = 0; k < order_.size(); ++k) {
        Body& B = bodies_[order_[k]];
        B.err = (p_->heat_on && B.units) ? p_->cpu.err_mult(B.err / B.units) : 1.0f;
    }
}

void Soup::run_body(u32 b, u64 tick) {
    double budget = static_cast<double>(bodies_[b].speed) * p_->instr_per_core / 1000.0 +
                    bodies_[b].carry;
    i64 n = budget > 0 ? static_cast<i64>(floor(budget)) : 0;
    bodies_[b].carry = static_cast<float>(budget - static_cast<double>(n));
    if (n <= 0)
        return;

    // The body's share splits equally among its running CPUs; the remainder
    // goes one each to CPUs from a tick-rotated offset.
    scratch_.clear();
    for (u32 c = bodies_[b].cpus; c != kNone; c = cpus_[c].next) {
        if (!cpus_[c].halted) {
            cpus_[c].yielded = 0;
            scratch_.push(c);
        }
    }
    const usize k = scratch_.size();
    if (k == 0)
        return;
    const i64 q = n / static_cast<i64>(k);
    const usize extra = static_cast<usize>(n % static_cast<i64>(k));
    const usize off = static_cast<usize>(tick % k);
    // scratch_ is a snapshot: CPUs an ENGULF adds run from next tick.
    for (usize i = 0; i < k; ++i) {
        i64 quota = q + (((i + k - off) % k) < extra ? 1 : 0);
        if (quota <= 0)
            continue;
        i64 used = run_cpu(scratch_[i], quota);
        Body& B = bodies_[b];
        B.work += static_cast<u32>(used);
        if (used > quota)
            B.carry -= static_cast<float>(used - quota); // attack debt
    }
}

void Soup::heat_update() {
    if (!p_->heat_on)
        return;
    const float decay = 1.0f - p_->cool;

    // CPU units: input from the owner's utilisation of its nominal share.
    for (usize k = 0; k < order_.size(); ++k) {
        Body& B = bodies_[order_[k]];
        float nominal = static_cast<float>(B.units) * p_->instr_per_core / 1000.0f;
        B.speed = nominal > 0 ? p_->cpu_gain * static_cast<float>(B.work) / nominal : 0.0f;
    }
    const u32 m = p_->millicores;
    for (u32 u = 0; u < m; ++u) {
        u32 o = unit_owner_[u];
        float h = unit_heat_[u] * decay + (o != kNone ? bodies_[o].speed : 0.0f);
        unit_heat_[u] = h;
        if (o != kNone && h > p_->cpu.max) {
            unlink_unit(u); // throttled to the limit: the unit is freed, hot
            pool_push(u);
            ++tally_.cpu_released;
        }
    }

    // Memory pages: fetches and writes heat the page they touch.
    const u32 pages = static_cast<u32>(page_heat_.size());
    for (u32 pg = 0; pg < pages; ++pg) {
        float h = page_heat_[pg] * decay + p_->fetch_gain * static_cast<float>(page_fetch_[pg]) +
                  p_->write_gain * static_cast<float>(page_write_[pg]);
        page_fetch_[pg] = 0;
        page_write_[pg] = 0;
        page_heat_[pg] = h;
        if (h > p_->mem.max) {
            // The bank shuts down: whatever it held becomes waste, keeping its
            // heat (and its contents, for scavengers).
            ++free_epoch_;
            u32 end = min_of((pg + 1) << kPageBits, p_->size);
            for (u32 a = pg << kPageBits; a < end; ++a) {
                u32 k = owner_[a];
                if (k == kNone)
                    continue;
                --bodies_[blocks_[k].body].mem;
                --blocks_[k].owned;
                owner_[a] = kNone;
                used_[a >> 6] &= ~(1ull << (a & 63));
                ++waste_;
                ++tally_.mem_released;
            }
        }
    }
}

// Background and heat-driven bit rot: each codon flips at rot_rate x the error
// multiplier of its page, per tick. Sampled exactly by thinning a Poisson
// process at the maximum rate, so the cost is per event, not per codon.
void Soup::bit_rot() {
    if (p_->rot_rate <= 0)
        return;
    const double top = p_->heat_on ? p_->mem.err_max : 1.0;
    const double rate = p_->rot_rate * top;
    double pos = -log(1.0 - rng_.unit()) / rate;
    while (pos < static_cast<double>(p_->size)) {
        u32 a = static_cast<u32>(pos);
        bool hit = true;
        if (p_->heat_on)
            hit = rng_.unit() * top < p_->mem.err_mult(page_heat_[a >> kPageBits]);
        if (hit) {
            mem_[a] ^= static_cast<u8>(1u << rng_.below(6));
            ++tally_.rot_flips;
        }
        pos += -log(1.0 - rng_.unit()) / rate;
    }
}

void Soup::death_sweep() {
    // A body dies when it has no running CPU: no un-halted CPU, or no
    // millicore to run on (6.4 #1). Engulfed bodies are already gone.
    for (usize k = 0; k < order_.size(); ++k) {
        u32 b = order_[k];
        Body& B = bodies_[b];
        if (!B.alive)
            continue;
        bool running = false;
        if (B.units > 0)
            for (u32 c = B.cpus; c != kNone && !running; c = cpus_[c].next)
                running = !cpus_[c].halted;
        if (!running)
            kill(b);
    }
    usize w = 0;
    for (usize k = 0; k < order_.size(); ++k) {
        u32 b = order_[k];
        if (bodies_[b].alive)
            order_[w++] = b;
        else
            free_bodies_.push(b);
    }
    order_.resize(w);
}

// ---------------------------------------------------------------------------
// State fingerprint, snapshots, invariants
// ---------------------------------------------------------------------------

void Soup::hash_into(Hasher& h) const {
    h.word(waste_);
    h.word(next_serial_);
    h.bytes(mem_.data(), mem_.size());
    h.bytes(owner_.data(), owner_.size() * sizeof(u32));
    h.bytes(blocks_.data(), blocks_.size() * sizeof(Block));
    h.bytes(free_blocks_.data(), free_blocks_.size() * sizeof(u32));
    h.bytes(bodies_.data(), bodies_.size() * sizeof(Body));
    h.bytes(free_bodies_.data(), free_bodies_.size() * sizeof(u32));
    h.bytes(cpus_.data(), cpus_.size() * sizeof(Cpu));
    h.bytes(free_cpus_.data(), free_cpus_.size() * sizeof(u32));
    h.bytes(order_.data(), order_.size() * sizeof(u32));
    h.bytes(unit_owner_.data(), unit_owner_.size() * sizeof(u32));
    h.bytes(unit_heat_.data(), unit_heat_.size() * sizeof(float));
    h.bytes(unit_next_.data(), unit_next_.size() * sizeof(u32));
    h.bytes(unit_prev_.data(), unit_prev_.size() * sizeof(u32));
    h.bytes(pool_.data(), pool_.size() * sizeof(u32));
    h.bytes(page_heat_.data(), page_heat_.size() * sizeof(float));
}

namespace {

template <class T> void put_array(ByteWriter& w, const Array<T>& a) {
    w.put_u32(static_cast<u32>(a.size()));
    w.put_bytes(a.data(), a.size() * sizeof(T));
}

template <class T> bool get_array(ByteReader& r, Array<T>* a, u64 max_count) {
    u32 n = 0;
    if (!r.get_u32(&n) || n > max_count || static_cast<u64>(n) * sizeof(T) > r.remaining())
        return false;
    a->resize(n);
    return r.get_bytes(a->data(), static_cast<usize>(n) * sizeof(T));
}

} // namespace

void Soup::save(ByteWriter& w) const {
    w.put_u32(waste_);
    w.put_u64(next_serial_);
    put_array(w, mem_);
    put_array(w, owner_);
    put_array(w, blocks_);
    put_array(w, free_blocks_);
    put_array(w, bodies_);
    put_array(w, free_bodies_);
    put_array(w, cpus_);
    put_array(w, free_cpus_);
    put_array(w, order_);
    put_array(w, unit_owner_);
    put_array(w, unit_heat_);
    put_array(w, unit_next_);
    put_array(w, unit_prev_);
    put_array(w, pool_);
    put_array(w, page_heat_);
}

bool Soup::load(ByteReader& r, TextBuf* err) {
    const u64 n = p_->size, m = p_->millicores, many = 0xFFFFFFFFull;
    bool ok = r.get_u32(&waste_) && r.get_u64(&next_serial_) && get_array(r, &mem_, n) &&
              get_array(r, &owner_, n) && get_array(r, &blocks_, many) &&
              get_array(r, &free_blocks_, many) && get_array(r, &bodies_, many) &&
              get_array(r, &free_bodies_, many) && get_array(r, &cpus_, many) &&
              get_array(r, &free_cpus_, many) && get_array(r, &order_, many) &&
              get_array(r, &unit_owner_, m) && get_array(r, &unit_heat_, m) &&
              get_array(r, &unit_next_, m) && get_array(r, &unit_prev_, m) &&
              get_array(r, &pool_, m) && get_array(r, &page_heat_, many);
    if (!ok || mem_.size() != n || owner_.size() != n || unit_owner_.size() != m ||
        page_heat_.size() != ((n + kPage - 1) >> kPageBits)) {
        err->append("snapshot: soup section truncated or the wrong shape");
        return false;
    }
    // Rebuild the derived structures, then demand every invariant.
    used_.resize((static_cast<usize>(n) + 63) / 64);
    used_.fill(0);
    for (u32 a = 0; a < n; ++a) {
        if (mem_[a] > 63 || (owner_[a] != kNone && owner_[a] >= blocks_.size())) {
            err->append("snapshot: soup memory or ownership out of range");
            return false;
        }
        if (owner_[a] != kNone)
            used_[a >> 6] |= 1ull << (a & 63);
    }
    pool_pos_.resize(m);
    pool_pos_.fill(kNone);
    for (u32 i = 0; i < pool_.size(); ++i) {
        if (pool_[i] >= m) {
            err->append("snapshot: soup pool out of range");
            return false;
        }
        pool_pos_[pool_[i]] = i;
    }
    page_fetch_.resize(page_heat_.size());
    page_fetch_.fill(0);
    page_write_.resize(page_heat_.size());
    page_write_.fill(0);
    TextBuf why;
    if (!check(&why)) {
        err->append("snapshot: soup invariant broken: ");
        err->append(why.view());
        return false;
    }
    return true;
}

bool Soup::check(TextBuf* why) const {
    const u32 n = p_->size;
    auto fail = [why](const char* msg, u64 at) {
        why->append(msg);
        why->append(" (");
        why->append_u64(at);
        why->append_char(')');
        return false;
    };
    // Memory: waste count, bitmap, block ownership counts.
    u32 waste = 0;
    for (u32 a = 0; a < n; ++a) {
        bool used = (used_[a >> 6] >> (a & 63)) & 1;
        if (used != (owner_[a] != kNone))
            return fail("used bitmap disagrees with owner map at", a);
        if (owner_[a] == kNone) {
            ++waste;
            continue;
        }
        const Block& bl = blocks_[owner_[a]];
        if (bl.body == kNone || bl.body >= bodies_.size() || !bodies_[bl.body].alive ||
            a < bl.start || a >= bl.start + bl.len)
            return fail("codon owned by a free block or outside its range", a);
    }
    if (waste != waste_)
        return fail("waste count wrong, actual", waste);

    // Bodies: lists and counts; order_ holds exactly the live ones, by serial.
    u64 mem_total = 0, units_total = 0;
    usize live = 0;
    for (u32 b = 0; b < bodies_.size(); ++b) {
        const Body& B = bodies_[b];
        if (!B.alive)
            continue;
        ++live;
        u32 mem = 0;
        usize steps = 0;
        for (u32 k = B.blocks; k != kNone; k = blocks_[k].next) {
            if (k >= blocks_.size() || ++steps > blocks_.size())
                return fail("block list out of range or cyclic at body", b);
            if (blocks_[k].body != b)
                return fail("block list crosses bodies at block", k);
            u32 owned = 0;
            for (u32 a = blocks_[k].start; a < blocks_[k].start + blocks_[k].len; ++a)
                owned += owner_[a] == k;
            if (owned != blocks_[k].owned)
                return fail("block owned count wrong at block", k);
            mem += owned;
        }
        if (mem != B.mem)
            return fail("body memory count wrong at body", b);
        mem_total += mem;
        u32 units = 0;
        for (u32 u = B.unit_head; u != kNone; u = unit_next_[u]) {
            if (u >= p_->millicores || units > p_->millicores)
                return fail("unit list out of range or cyclic at body", b);
            if (unit_owner_[u] != b)
                return fail("unit list crosses bodies at unit", u);
            ++units;
        }
        if (units != B.units)
            return fail("body unit count wrong at body", b);
        units_total += units;
        u32 ncpu = 0;
        for (u32 c = B.cpus; c != kNone; c = cpus_[c].next) {
            if (c >= cpus_.size() || ncpu > cpus_.size())
                return fail("CPU list out of range or cyclic at body", b);
            if (cpus_[c].body != b)
                return fail("CPU list crosses bodies at CPU", c);
            if (cpus_[c].block != kNone && blocks_[cpus_[c].block].body != b)
                return fail("CPU wrap block not owned by its body, CPU", c);
            ++ncpu;
        }
        if (ncpu != B.ncpu)
            return fail("body CPU count wrong at body", b);
    }
    if (mem_total + waste_ != n)
        return fail("memory not conserved: owned", mem_total);
    if (units_total + pool_.size() != p_->millicores)
        return fail("CPU units not conserved: owned", units_total);
    for (u32 i = 0; i < pool_.size(); ++i)
        if (unit_owner_[pool_[i]] != kNone || pool_pos_[pool_[i]] != i)
            return fail("pool entry inconsistent at", i);
    if (live != order_.size())
        return fail("schedule order size wrong, live bodies", live);
    for (usize k = 0; k < order_.size(); ++k) {
        if (!bodies_[order_[k]].alive)
            return fail("schedule order holds a dead body at", k);
        if (k > 0 && bodies_[order_[k]].serial <= bodies_[order_[k - 1]].serial)
            return fail("schedule order not by serial at", k);
    }
    return true;
}

} // namespace nova
