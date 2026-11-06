#include "nova/soup.h"

#include <intrin.h> // _BitScanForward64, _BitScanReverse64

#include <cmath>  // floor, log, pow
#include <cstdio> // snprintf

#include "nova/lineage.h"

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

// A uniform random permutation of 0..n-1 with map[0] != 0: the shuffled
// control must actually move the self variant (Phase 1b D6), so a draw that
// leaves it at 0 is redrawn.
void draw_selector_map(Rng& r, u8* map, u32 n) {
    do {
        for (u32 i = 0; i < n; ++i)
            map[i] = static_cast<u8>(i);
        for (u32 i = n - 1; i > 0; --i) {
            u32 j = static_cast<u32>(r.below(i + 1));
            u8 t = map[i];
            map[i] = map[j];
            map[j] = t;
        }
    } while (map[0] == 0);
}

void identity_map(u8* map, u32 n) {
    for (u32 i = 0; i < n; ++i)
        map[i] = static_cast<u8>(i);
}

void append_map(TextBuf& out, const char* name, const u8* map, u32 n) {
    out.append_char(' ');
    out.append(name);
    out.append_char('=');
    for (u32 i = 0; i < n; ++i) {
        if (i)
            out.append_char(',');
        out.append_u64(map[i]);
    }
}

} // namespace

void describe_selectors(const SelectorMap& m, TextBuf& out) {
    if (!m.shuffled) {
        out.append("nearest");
        return;
    }
    out.append("shuffled");
    append_map(out, "sense", m.sense, m.sense_n);
    append_map(out, "res", m.res, m.res_n);
    append_map(out, "head", m.head, 2);
    append_map(out, "divide", m.divide, 4);
    // The op maps exist only with their unit; unshuffled (identity) ones are left out.
    bool gpu_drawn = false, npu_drawn = false;
    for (u32 i = 0; i < kGpuOps; ++i)
        gpu_drawn = gpu_drawn || m.gpu[i] != i;
    for (u32 i = 0; i < kNpuOps; ++i)
        npu_drawn = npu_drawn || m.npu[i] != i;
    const bool disk_drawn = m.disk[0] != 0 || m.disk[1] != 1 || m.disk[2] != 2;
    if (disk_drawn)
        append_map(out, "disk", m.disk, 3);
    if (gpu_drawn)
        append_map(out, "gpu", m.gpu, kGpuOps);
    if (npu_drawn)
        append_map(out, "npu", m.npu, kNpuOps);
}

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

    // The in-world GPU (Phase 2, design.md 6.7). No units = no GPU: every GPU
    // selector fails as in Phase 1, and nothing else changes.
    // The real devices (Phase 5, design.md 6.11): asynchronous, batched ops,
    // with a high-volume profile by default (Damian: GPUs are built for
    // volume, so expensive per op and nearly free per element). Instant mode
    // is exactly Phase 4.
    Str mode = cfg.get_str("accel.mode", "instant");
    p->accel_async = mode.eq("async");
    if (!p->accel_async && !mode.eq("instant"))
        cfg.fail("accel.mode", "must be instant or async");
    p->accel_latency = get_u32(cfg, "accel.latency", 2, 1, 64);
    const bool hv = p->accel_async;
    p->gpus = get_u32(cfg, "world.gpus", 0, 0, 10000000);
    p->gpu_elements = get_u32(cfg, "gpu.elements", hv ? 4096 : 32, 1, 100000000);
    p->gpu_setup = get_u32(cfg, "gpu.setup", hv ? 32 : 4, 0, 1000000);
    p->gpu_cpu_per_element = get_f64(cfg, "gpu.cpu_per_element", hv ? 1.0 / 1024.0 : 0.0625, 0, 1e6);
    p->gpu_wear = get_f64(cfg, "gpu.wear", 0.0, 0, 1);
    // The in-world NPU (Phase 2b, design.md 6.8), the same way.
    p->npus = get_u32(cfg, "world.npus", 0, 0, 10000000);
    p->npu_macs = get_u32(cfg, "npu.macs", hv ? 65536 : 256, 1, 1000000000);
    p->npu_cpu_per_mac = get_f64(cfg, "npu.cpu_per_mac", hv ? 1.0 / 16384.0 : 1.0 / 64.0, 0, 1e6);
    p->npu_max_matvec = hv ? 256 : 64;
    p->npu_max_matmul = hv ? 64 : 32;
    p->npu_wear = get_f64(cfg, "npu.wear", 0.0, 0, 1);

    // Phase 1b rules (design.md 6.6). mem.upkeep = 0 is exactly Phase 1a.
    p->upkeep = get_f64(cfg, "mem.upkeep", 2.0, 0, 1e9);
    p->upkeep_exp = get_f64(cfg, "mem.upkeep_exp", 4.0 / 3.0, 0.5, 4.0);
    p->cpu_wear = get_f64(cfg, "cpu.wear", 0.0, 0, 1);
    Str timing = cfg.get_str("spark.timing", "first");
    if (timing.eq("first"))
        p->spark_late = false;
    else if (timing.eq("last"))
        p->spark_late = true;
    else
        cfg.fail("spark.timing", "must be first or last");

    Str table = cfg.get_str("isa.table", "nova_v1");
    if (table.eq("custom")) {
        Str codons = cfg.get_str("isa.codons", "");
        TextBuf err;
        if (!parse_table(codons, &p->table, &err))
            cfg.fail("isa.codons", err.c_str());
    } else if (!builtin_table(table, &p->table)) {
        cfg.fail("isa.table", "must be nova_v1, nova_v1_plain, nova_v1_inv2, tierra0 or custom");
    }

    // Selector numbering (D6): nearest-to-self, or a per-run shuffle drawn from
    // isa.selector_seed (0 = the world seed, so paired runs differ by seed).
    Str sel = cfg.get_str("isa.selectors", "nearest");
    u64 sel_seed = cfg.get_u64("isa.selector_seed", 0);
    // Dispersal (Phase 3): DIVIDE to a neighbour, on by default only in
    // procedural worlds, so ring worlds stay exactly as before.
    const Str graph = cfg.get_str("world.graph", "ring");
    const bool spatial = graph.eq("procedural") || graph.eq("landscape"); // Phase 7: landscapes too
    p->dispersal = cfg.get_bool("world.dispersal", spatial);
    // The disk tier and volatility (Phase 4): no disk = everything disk fails,
    // as before; no volatility = no wipes.
    p->disk = get_u32(cfg, "world.disk", 0, 0, 1u << 24);
    p->disk_cost_base = get_u32(cfg, "disk.cost_base", 4, 0, 1000000);
    p->disk_cost_per_codon = get_f64(cfg, "disk.cost_per_codon", 0.25, 0, 1e6);
    p->disk_rot = get_f64(cfg, "disk.rot", p->disk ? 1e-6 : 0.0, 0, 1);
    p->volatility = get_f64(cfg, "mem.volatility", 0.0, 0, 1);
    p->wipe_pages = get_u32(cfg, "mem.wipe_pages", 4, 1, 1u << 18);
    // Remnants and salvage (Phase 4, Damian): 0 = off, every read as before.
    p->salvage_ratio = get_f64(cfg, "salvage.ratio", 0.0, 0, 1e6);
    // The signal channel (Phase 4): 0 channels = EMIT and LISTEN fail.
    p->signal_channels = get_u32(cfg, "signal.channels", 0, 0, 256);
    p->signal_cost_base = get_u32(cfg, "signal.cost_base", 2, 0, 1000000);
    p->signal_cost_per_amp = get_f64(cfg, "signal.cost_per_amp", 1.0 / 32.0, 0, 1e6);
    p->signal_floor = get_f64(cfg, "signal.floor", 1.0, 0, 1e9);
    // The node lifecycle (Phase 4): endurance 0 and starve_ticks 0 = off.
    p->endurance = get_f64(cfg, "node.endurance", 0.0, 0, 1e18);
    p->decline = get_f64(cfg, "node.decline", 4.0, 0, 1e6);
    p->starve_below = get_f64(cfg, "node.starve_below", 0.002, 0, 1);
    p->starve_ticks = get_u32(cfg, "node.starve_ticks", 0, 0, 1000000000);
    // The meter ebbs like heat (Damian): constantly starving it settles at
    // 1 / ebb, so it reaches starve_ticks only if the node starves more than
    // starve_ticks x ebb of the time (half, at the defaults).
    p->starve_ebb = get_f64(cfg, "node.starve_ebb", 0.00025, 0, 1);

    // Measured costs (Phase 9, design.md 6.15): classic = exactly Phase 8.
    const Str ct = cfg.get_str("cost.table", "classic");
    if (!builtin_costs(ct, &p->cost)) {
        cfg.fail("cost.table", "must be classic or home_v1");
        builtin_costs("classic", &p->cost);
    }
    p->plain = p->cost.timed ? static_cast<double>(p->cost.alu) : 1.0;
    p->tick_units = static_cast<double>(p->instr_per_core) * p->plain;
    p->engulf_cpt = get_u32(cfg, "engulf.codons_per_tick", 16, 1, 1u << 24);
    p->inject_cpt = get_u32(cfg, "inject.codons_per_tick", 16, 1, 1u << 24);

    SelectorMap& m = p->sel;
    m = SelectorMap{};
    // SENSE has the readings up to the highest enabled one; a reading of a
    // feature that's off fails.
    u32 sense_n = kSenseReadings;
    if (p->gpus)
        sense_n = kSenseReadingsGpu;
    if (p->npus)
        sense_n = kSenseReadingsNpu;
    if (p->dispersal)
        sense_n = kSenseReadingsWorld;
    if (p->disk)
        sense_n = kSenseReadingsDisk;
    if (p->endurance > 0 || p->starve_ticks > 0)
        sense_n = kSenseReadingsLife; // + node health
    if (p->accel_async)
        sense_n = kSenseReadingsAsync; // + my last accelerator result, my ops pending

    // Warps (Phase 8, design.md 6.14): off = everything exactly as Phase 7.
    p->warps = cfg.get_bool("gpu.warps", false);
    p->warp_lanes = get_u32(cfg, "warp.lanes", 32, 1, kMaxLanes);
    p->warp_max = get_u32(cfg, "warp.max", 4, 1, 64);
    p->warp_capacity = get_u32(cfg, "warp.capacity", 256, 1, 100000000);
    p->warp_step_cost = get_u32(cfg, "warp.step_cost", 16, 0, 1000000);
    p->warp_lane_cost = get_u32(cfg, "warp.lane_cost", 1, 0, 1000000);
    p->warp_rounds = get_u32(cfg, "warp.rounds", 1, 1, 64);
    p->spark_gpu_grant = get_u32(cfg, "spark.gpu_grant", 0, 0, 10000000);
    p->spark_gpu_born = get_f64(cfg, "spark.gpu_born", 0.5, 0, 1);
    if (p->warps && p->warp_step_cost + p->warp_lane_cost == 0)
        cfg.fail("warp.step_cost", "warp.step_cost and warp.lane_cost can't both be 0");
    // Phase 9b (design.md 6.15): with a timed table the devices' capacities
    // come from it, not from these settings. A GPU unit is one of the GPU's
    // lanes: a tick of its time, at gpu_thr a lane-op. A warp step fills the
    // warp's whole width whatever lanes it runs (lanes that branched away
    // still hold their slots), so it costs warp.lanes lane-ops. An NPU unit is
    // a thousandth of the NPU.
    if (p->cost.timed) {
        const double tick = p->tick_units;
        auto clamp = [](double v) { return v < 1 ? 1u : (v > 4e9 ? 4000000000u : static_cast<u32>(v)); };
        p->warp_capacity = clamp(tick);
        p->warp_step_cost = clamp(static_cast<double>(p->warp_lanes) * static_cast<double>(p->cost.gpu_thr));
        p->warp_lane_cost = 0;
        p->gpu_elements = clamp(tick / static_cast<double>(p->cost.gpu_thr));
        p->npu_macs = clamp(tick / (static_cast<double>(p->cost.npu_mac) * 1000.0));
    }
    if (p->warps)
        sense_n = kSenseReadingsWarp; // + my lane, lanes in my warp
    if (p->cost.timed)
        sense_n = kSenseReadingsActions; // + an action on me: ticks until it ends, what it is
    m.sense_n = static_cast<u8>(sense_n);
    m.res_n = static_cast<u8>(p->npus ? 5 : 4);
    identity_map(m.sense, kSenseReadingsMax);
    identity_map(m.disk, 3);
    identity_map(m.res, 5);
    identity_map(m.head, 2);
    identity_map(m.divide, 4);
    identity_map(m.gpu, kGpuOps);
    identity_map(m.npu, kNpuOps);
    if (sel.eq("shuffled")) {
        const u64 world_seed = cfg.get_u64("seed", 1);
        if (sel_seed == 0)
            sel_seed = world_seed;
        Rng r(sel_seed, Rng::kWorldNode, 0x73656C6563746F72ull); // "selector"
        // Without a GPU or NPU the draws are exactly Phase 1b's.
        draw_selector_map(r, m.sense, m.sense_n);
        draw_selector_map(r, m.res, m.res_n);
        draw_selector_map(r, m.head, 2);
        draw_selector_map(r, m.divide, 4);
        if (p->gpus)
            draw_selector_map(r, m.gpu, kGpuOps); // compare (the no-op) never stays at 0
        if (p->npus)
            draw_selector_map(r, m.npu, kNpuOps);
        if (p->disk) // compare (changes nothing) never stays at 0
            draw_selector_map(r, m.disk, 3);
        m.shuffled = 1;
    } else if (!sel.eq("nearest")) {
        cfg.fail("isa.selectors", "must be nearest or shuffled");
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
    // The GPU runs hot: flat out it settles at 90, deep in its throttle band.
    p->gpu_gain = static_cast<float>(get_f64(cfg, "heat.gpu_gain", 9.0, 0, 1e9));
    p->gpu = get_curve(cfg, "heat.gpu", HeatCurve{40, 60, 100, 0.25f, 10, 0});
    p->npu_gain = static_cast<float>(get_f64(cfg, "heat.npu_gain", 9.0, 0, 1e9));
    p->npu = get_curve(cfg, "heat.npu", HeatCurve{40, 60, 100, 0.25f, 10, 0});
    // The ambient leak (Phase 4): 0 = off, every unit's heat its own.
    p->ambient = static_cast<float>(get_f64(cfg, "heat.ambient", 0.0, 0, 1e6));

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
    // INJECT needs a weakness to exploit and a host it fits in (Phase 1b,
    // design.md 6.6). false / 0 restore the Phase 1a contest for INJECT.
    p->inject_key = cfg.get_bool("contact.inject_key", true);
    p->inject_room = get_f64(cfg, "contact.inject_room", 1.0, 0, 1e6);

    Str genome = cfg.get_str("seed.genome", "");
    p->seed_at = get_u32(cfg, "seed.at", 0, 0, soup_size - 1);
    p->seed_millicores = get_u32(cfg, "seed.millicores", 1000, 0, 10000000);
    p->seed_gpus = get_u32(cfg, "seed.gpus", 0, 0, 10000000);
    p->seed_npus = get_u32(cfg, "seed.npus", 0, 0, 10000000);
    const Str form = cfg.get_str("seed.form", "cpu");
    p->seed_form = kFormCpu;
    if (form.eq("gpu"))
        p->seed_form = kFormGpu;
    else if (!form.eq("cpu"))
        cfg.fail("seed.form", "must be cpu or gpu");
    if (p->seed_form == kFormGpu && !p->warps)
        cfg.fail("seed.form", "gpu needs gpu.warps = true");
    p->seed_len = 0;
    if (cfg.ok() && !genome.empty()) {
        TextBuf err;
        Array<u8> code;
        if (!genome_from_names(p->table, genome, &code, &err)) {
            cfg.fail("seed.genome", err.c_str());
        } else if (code.size() > kMaxSeedLen) {
            cfg.fail("seed.genome", "longer than 1024 codons");
        } else if (code.size() > soup_size - p->seed_at) {
            cfg.fail("seed.genome", "does not fit in the soup at seed.at");
        } else {
            for (usize i = 0; i < code.size(); ++i)
                p->seed_genome[i] = code[i];
            p->seed_len = static_cast<u32>(code.size());
        }
    }
    return cfg.ok();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void Soup::init(const SoupParams* p, u64 seed, u32 node, u32 life, const Ruins* ruins) {
    p_ = p;
    seed_ = seed;
    node_ = node;
    // A detector sized for an earlier shape must not watch the new one: the
    // caller attaches it again afterwards (set_observer sizes it).
    obs_ = nullptr;
    rng_ = Rng(seed, node, Rng::kInitTick - life); // life 0: exactly as before Phase 4
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
    actions_.clear(); // Phase 9

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

    // GPU and NPU units: every one starts in the pool, cold (none without one).
    gpu_.init(p->gpus);
    npu_.init(p->npus);
    // The disk (Phase 4): random contents, all free, from its own stream so the
    // memory and sparks above are drawn exactly as without one.
    if (p->disk) {
        // Phase 7b: on real disk, in this node's files (bound once, kept for
        // every life of the node).
        if (disk_folder_ && !disk_.mem.in_file() && !disk_.bind_files(disk_folder_, node))
            NOVA_FATAL("Soup: cannot create the disk tier's files in disk.path");
        Rng dr(seed ^ 0x6469736B00000000ull, node, Rng::kInitTick - life); // "disk"
        disk_.init(p->disk, dr);
    } else {
        disk_.init(0, rng_);
    }
    // Signals (Phase 4): nothing emitted or heard yet.
    emit_.resize(p->signal_channels);
    emit_.fill(0);
    field_.resize(p->signal_channels);
    field_.fill(0);
    emit_count_.resize(p->signal_channels);
    emit_count_.fill(0);
    emit_amp_.resize(p->signal_channels);
    emit_amp_.fill(0);
    // Salvage stamps (Phase 4): nobody's code yet.
    stamp_.clear();
    disk_.stamp.clear();
    if (p->salvage_ratio > 0) {
        stamp_.resize(n);
        disk_.stamp.resize(p->disk);
    }
    // Asynchronous accelerator ops (Phase 5): nothing issued yet.
    issued_.clear();
    issue_ref_.clear();
    issue_addr_.clear();
    accel_serial_ = 0;
    cpu_gen_.clear();
    cpu_result_.clear();
    cpu_pending_.clear();
    // Warps (Phase 8): none yet.
    warps_.clear();
    free_warps_.clear();
    lanes_.clear();
    warp_head_.clear();
    warp_count_.clear();
    warp_carry_.clear();
    warp_upkeep_.clear();
    wleft_.clear();
    wsteps_.clear();
    wtime_.clear();
    wuntil_.clear();
    gpu_batch_.clear();
    npu_batch_.clear();
    wagain_.clear();
    wlive_.clear();
    wact_.clear();
    wstate_.clear();
    wchain_ = 0;
    wcst_.clear();
    if (p->warps)
        build_warp_consts();
    // The lifecycle (Phase 4): a fresh life.
    writes_ = 0;
    starve_meter_ = 0;
    collapse_ = false;
    hazard_mult_ = 1.0;
    // Ruins of an earlier life (Damian): laid over the new soup where the
    // sizes overlap, stamps and all, then weathered by each store's rot rate
    // over the fallow time (a Poisson process of bit flips, as rot itself is;
    // past 8 expected flips a codon, simply random).
    if (ruins) {
        Rng wr(seed ^ 0x7275696E73000000ull, node, life); // "ruins"
        auto weather = [&wr](u8* m, u32 len, double lambda) {
            if (lambda <= 0 || len == 0)
                return;
            if (lambda >= 8) {
                for (u32 i = 0; i < len; ++i)
                    m[i] = static_cast<u8>(wr.next() & 63);
                return;
            }
            double pos = -log(1.0 - wr.unit()) / lambda;
            while (pos < len) {
                m[static_cast<u32>(pos)] ^= static_cast<u8>(1u << wr.below(6));
                pos += -log(1.0 - wr.unit()) / lambda;
            }
        };
        const u32 mo = min_of(n, static_cast<u32>(ruins->mem.size()));
        for (u32 i = 0; i < mo; ++i)
            mem_[i] = ruins->mem[i];
        if (!stamp_.empty())
            for (u32 i = 0; i < mo && i < ruins->mem_stamp.size(); ++i)
                stamp_[i] = ruins->mem_stamp[i];
        weather(mem_.data(), mo, p->rot_rate * static_cast<double>(ruins->fallow));
        const u32 dn = disk_.size();
        const u32 dov = min_of(dn, static_cast<u32>(ruins->disk.size()));
        for (u32 i = 0; i < dov; ++i)
            disk_.mem[i] = ruins->disk[i];
        if (!disk_.stamp.empty())
            for (u32 i = 0; i < dov && i < ruins->disk_stamp.size(); ++i)
                disk_.stamp[i] = ruins->disk_stamp[i];
        weather(disk_.mem.data(), dov, p->disk_rot * static_cast<double>(ruins->fallow));
    }

    tally_ = {};
    clear_departures();
    if (p->seed_len) {
        u32 len = p->seed_len;
        for (u32 i = 0; i < len; ++i)
            mem_[p->seed_at + i] = p->seed_genome[i];
        u32 b = kNone;
        if (p->seed_form == kFormGpu) // born on the GPU (Phase 8)
            b = create_body_form(p->seed_at, len, p->seed_millicores, p->seed_gpus, kFormGpu);
        else
            b = create_body(p->seed_at, len, p->seed_millicores);
        if (b != kNone && p->seed_gpus && p->seed_form != kFormGpu)
            claim_gpus(b, p->seed_gpus);
        if (b != kNone && p->seed_npus)
            claim_npus(b, p->seed_npus);
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
u32 Soup::find_free_fwd(u32 from, u32 len) const { return free_run_fwd(used_.data(), p_->size, from, len); }

// The same over any bitmap of n positions (memory, and the disk in Phase 4).
u32 free_run_fwd(const u64* used_, u32 n, u32 from, u32 len) {
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
    if (!stamp_.empty()) // a claim makes it wholly the claimer's (Damian)
        for (u32 a = start; a < start + len; ++a)
            stamp_[a] = 0;
    mark_used(start, len, true);
    waste_ -= len;
    if (obs_)
        obs_->on_new_block(k, start, len);
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

void Soup::stamp_block(u32 k, u32 g) {
    if (stamp_.empty())
        return;
    const Block& bl = blocks_[k];
    for (u32 a = bl.start; a < bl.start + bl.len; ++a)
        if (owner_[a] == k && stamp_[a] == 0)
            stamp_[a] = g;
}

void Soup::free_block(u32 k) {
    if (obs_)
        obs_->on_free_block(k, tick_);
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
    // (A lane's block is set afresh every step before it is read, so lanes
    // keep theirs: they may sit on the GPU, out of the host's reach.)
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
    if (p_->gpus)
        gpu_.body_slot(b, bodies_.size());
    if (p_->npus)
        npu_.body_slot(b, bodies_.size());
    if (p_->disk)
        disk_.body_slot(b, bodies_.size());
    if (p_->warps) { // Phase 8: no warps yet, nothing carried
        if (warp_head_.size() < bodies_.size()) {
            warp_head_.resize(bodies_.size());
            warp_count_.resize(bodies_.size());
            warp_carry_.resize(bodies_.size());
            warp_upkeep_.resize(bodies_.size());
        }
        warp_head_[b] = kNone;
        warp_count_[b] = 0;
        warp_carry_[b] = 0.0f;
        warp_upkeep_[b] = 0.0;
    }
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
    if (p_->accel_async) { // a new CPU in this slot: earlier results aren't its (Phase 5)
        if (cpu_gen_.size() < cpus_.size()) {
            cpu_gen_.resize(cpus_.size());
            cpu_result_.resize(cpus_.size());
            cpu_pending_.resize(cpus_.size());
        }
        ++cpu_gen_[c];
        cpu_result_[c] = 0;
        cpu_pending_[c] = 0;
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

void Soup::clear_departures() {
    departures_.clear();
    departure_codons_.clear();
}

// Phase 3 arrival (design.md 6.9), at the barrier after tick_: a random waste
// run of her length, found as sparks find theirs, from this node's stream
// (continuing where the tick left it, in the fixed delivery order).
// Phase 8: a body on [start, start + len) with `units` millicores and `gpus`
// GPU units from the pools, born in `form` (or the other form if she holds
// only that one's units).
u32 Soup::create_body_form(u32 start, u32 len, u32 units, u32 gpus, u32 form) {
    if (!is_free(start, len))
        return kNone;
    u32 b = new_body();
    new_block(start, len, b);
    claim_units(b, units);
    if (gpus)
        claim_gpus(b, gpus);
    birth(b, start, form);
    return b;
}

bool Soup::arrive(const u8* code, u32 len, u32 millicores, u32 gpus, u32 npus, u32 disk, const CargoTag& tag,
                  u32 form) {
    if (len == 0 || len > p_->size) {
        ++tally_.lost;
        return false;
    }
    const u32 from = static_cast<u32>(rng_.below(p_->size));
    u32 at = known_full(len) ? kNone : find_free_fwd(from, len);
    if (at == kNone && !known_full(len))
        at = find_free_fwd(0, len);
    if (at == kNone) {
        note_full(len);
        ++tally_.lost;
        return false;
    }
    for (u32 i = 0; i < len; ++i)
        mem_[at + i] = code[i];
    u32 b = kNone;
    if (!p_->warps) {
        b = create_body(at, len, millicores);
        NOVA_ASSERT(b != kNone);
        if (gpus && p_->gpus)
            claim_gpus(b, gpus);
    } else { // Phase 8: born in her form, once she holds her shares
        b = new_body();
        new_block(at, len, b);
        claim_units(b, millicores);
        if (gpus && p_->gpus)
            claim_gpus(b, gpus);
    }
    if (npus && p_->npus)
        claim_npus(b, npus);
    if (disk && p_->disk) // her disk share, empty, capped by what's free (Phase 4)
        disk_.claim_any(rng_, b, disk);
    if (p_->warps)
        birth(b, at, form);
    if (obs_)
        obs_->on_arrive(*this, owner_[at], tag, tick_);
    ++tally_.arrived;
    return true;
}

void Soup::kill(u32 b) {
    Body& B = bodies_[b];
    const u32 g = genome_len(b); // its remnants remember whose code they were
    for (u32 k = B.blocks; k != kNone;) {
        u32 next = blocks_[k].next;
        stamp_block(k, g);
        free_block(k);
        k = next;
    }
    while (B.unit_head != kNone) {
        u32 u = B.unit_head;
        unlink_unit(u);
        pool_push(u);
    }
    if (p_->gpus) // with their heat, like millicores
        gpu_.release(b, gpu_.count[b]);
    if (p_->npus)
        npu_.release(b, npu_.count[b]);
    if (p_->disk) // the disk is freed; its contents stay as fossils
        disk_.release_tail(b, disk_.total[b], g);
    if (p_->warps) // Phase 8: its warps end with it
        kill_warps(b);
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
// GPU and NPU units (Phase 2 and 2b, design.md 6.7-6.8): the millicore
// ledger again, once per unit kind
// ---------------------------------------------------------------------------

void UnitLedger::init(u32 n) {
    owner.resize(n);
    owner.fill(kNone);
    heat.resize(n);
    heat.fill(0.0f);
    next.resize(n);
    next.fill(kNone);
    prev.resize(n);
    prev.fill(kNone);
    pool.resize(n);
    pool_pos.resize(n);
    for (u32 u = 0; u < n; ++u) {
        pool[u] = u;
        pool_pos[u] = u;
    }
    head.clear();
    tail.clear();
    count.clear();
    cap.clear();
    done.clear();
    err.clear();
}

void UnitLedger::body_slot(u32 body, usize nbodies) {
    if (head.size() < nbodies) {
        head.resize(nbodies);
        tail.resize(nbodies);
        count.resize(nbodies);
        cap.resize(nbodies);
        done.resize(nbodies);
        err.resize(nbodies);
    }
    head[body] = kNone;
    tail[body] = kNone;
    count[body] = 0;
    cap[body] = 0.0f;
    done[body] = 0;
    err[body] = 1.0f;
}

void UnitLedger::pool_push(u32 u) {
    owner[u] = kNone;
    pool_pos[u] = static_cast<u32>(pool.size());
    pool.push(u);
}

void UnitLedger::pool_remove(u32 u) {
    u32 i = pool_pos[u];
    u32 last = pool.back();
    pool[i] = last;
    pool_pos[last] = i;
    pool.pop();
    pool_pos[u] = kNone;
}

void UnitLedger::append(u32 body, u32 u) {
    owner[u] = body;
    prev[u] = tail[body];
    next[u] = kNone;
    if (tail[body] != kNone)
        next[tail[body]] = u;
    else
        head[body] = u;
    tail[body] = u;
    ++count[body];
}

void UnitLedger::unlink(u32 u) {
    const u32 body = owner[u];
    u32 p = prev[u], n = next[u];
    if (p != kNone)
        next[p] = n;
    else
        head[body] = n;
    if (n != kNone)
        prev[n] = p;
    else
        tail[body] = p;
    prev[u] = next[u] = kNone;
    owner[u] = kNone;
    --count[body];
}

u32 UnitLedger::claim(Rng& rng, u32 body, u32 k) {
    if (k > pool.size())
        k = static_cast<u32>(pool.size());
    for (u32 i = 0; i < k; ++i) {
        u32 u = pool[rng.below(pool.size())]; // no favourites, hot or cold
        pool_remove(u);
        append(body, u);
    }
    return k;
}

void UnitLedger::move(u32 from, u32 to, u32 k) {
    for (u32 i = 0; i < k && head[from] != kNone; ++i) {
        u32 u = head[from];
        unlink(u);
        append(to, u);
    }
}

void UnitLedger::release(u32 body, u32 k) {
    for (u32 i = 0; i < k && head[body] != kNone; ++i) {
        u32 u = head[body];
        unlink(u);
        pool_push(u); // keeps its heat
    }
}

// Each live body's capacity this tick: per_unit per held unit, scaled by the
// unit's heat speed factor; and the copy-error multiplier from their mean heat.
// Bodies born later in the tick, and units gained mid-tick, count from the next.
void UnitLedger::prepare(const Array<u32>& order, const HeatCurve& curve, bool heat_on, u32 per_unit) {
    for (usize k = 0; k < order.size(); ++k) {
        const u32 b = order[k];
        cap[b] = 0.0f;
        done[b] = 0;
        err[b] = 0.0f; // heat sum for now
    }
    for (u32 u = 0; u < units(); ++u) {
        const u32 o = owner[u];
        if (o == kNone)
            continue;
        cap[o] += heat_on ? curve.speed(heat[u]) : 1.0f;
        err[o] += heat[u];
    }
    for (usize k = 0; k < order.size(); ++k) {
        const u32 b = order[k];
        err[b] = (heat_on && count[b]) ? curve.err_mult(err[b] / static_cast<float>(count[b])) : 1.0f;
        cap[b] *= static_cast<float>(per_unit);
    }
}

// Each unit gains gain x its owner's utilisation (work done / nominal
// capacity) and cools like everything else; past max it is freed, hot.
u32 UnitLedger::heat_update(const HeatCurve& curve, float gain, float decay, u32 per_unit, float ambient) {
    u32 freed = 0;
    for (u32 u = 0; u < units(); ++u) {
        const u32 o = owner[u];
        float input = 0.0f;
        if (o != kNone) {
            const float nominal = static_cast<float>(count[o]) * static_cast<float>(per_unit);
            input = nominal > 0 ? gain * static_cast<float>(done[o]) / nominal : 0.0f;
        }
        float h = heat[u] * decay + input;
        if (ambient > 0)
            h += ambient;
        heat[u] = h;
        if (o != kNone && h > curve.max) {
            unlink(u);
            pool_push(u);
            ++freed;
        }
    }
    return freed;
}

// Wear: held units fail back into the pool at rate f a tick, sampled as a
// Poisson process over the unit index, like CPU wear.
u32 UnitLedger::wear(Rng& rng, double f) {
    u32 worn = 0;
    const double m = static_cast<double>(units());
    double pos = -log(1.0 - rng.unit()) / f;
    while (pos < m) {
        u32 u = static_cast<u32>(pos);
        if (owner[u] != kNone) {
            unlink(u);
            pool_push(u);
            ++worn;
        }
        pos += -log(1.0 - rng.unit()) / f;
    }
    return worn;
}

void UnitLedger::hash_into(Hasher& h) const {
    h.bytes(owner.data(), owner.size() * sizeof(u32));
    h.bytes(heat.data(), heat.size() * sizeof(float));
    h.bytes(next.data(), next.size() * sizeof(u32));
    h.bytes(prev.data(), prev.size() * sizeof(u32));
    h.bytes(pool.data(), pool.size() * sizeof(u32));
    h.bytes(head.data(), head.size() * sizeof(u32));
    h.bytes(tail.data(), tail.size() * sizeof(u32));
    h.bytes(count.data(), count.size() * sizeof(u32));
}

u32 Soup::claim_gpus(u32 body, u32 k) {
    return p_->gpus ? gpu_.claim(rng_, body, k) : 0;
}

u32 Soup::claim_npus(u32 body, u32 k) {
    return p_->npus ? npu_.claim(rng_, body, k) : 0;
}

void Soup::prepare_gpu() {
    if (p_->gpus)
        gpu_.prepare(order_, p_->gpu, p_->heat_on, p_->gpu_elements);
    if (p_->npus)
        npu_.prepare(order_, p_->npu, p_->heat_on, p_->npu_macs);
}

void Soup::accel_heat_update() {
    if (!p_->heat_on)
        return;
    const float decay = 1.0f - p_->cool;
    if (p_->gpus)
        tally_.gpu_released += gpu_.heat_update(p_->gpu, p_->gpu_gain, decay, p_->gpu_elements, ambient_);
    if (p_->npus)
        tally_.npu_released += npu_.heat_update(p_->npu, p_->npu_gain, decay, p_->npu_macs, ambient_);
}

void Soup::accel_wear() {
    if (p_->gpus && p_->gpu_wear > 0)
        tally_.gpu_worn += gpu_.wear(rng_, p_->gpu_wear);
    if (p_->npus && p_->npu_wear > 0)
        tally_.npu_worn += npu_.wear(rng_, p_->npu_wear);
}

// ---------------------------------------------------------------------------
// The disk tier (Phase 4, design.md 6.10)
// ---------------------------------------------------------------------------

bool DiskLedger::bind_files(const char* folder, u32 node) {
    char path[600];
    const char* const names[4] = {"mem", "stamp", "owner", "used"};
    for (u32 i = 0; i < 4; ++i) {
        snprintf(path, sizeof path, "%s\\n%u.%s", folder, node, names[i]);
        const bool ok = i == 0 ? mem.bind(path) : (i == 1 ? stamp.bind(path) : (i == 2 ? owner.bind(path) : used.bind(path)));
        if (!ok)
            return false;
    }
    return true;
}

void DiskLedger::init(u32 n, Rng& rng) {
    mem.resize(n);
    for (u32 i = 0; i < n; ++i)
        mem[i] = static_cast<u8>(rng.next() & 63);
    owner.resize(n);
    owner.fill(kNone);
    used.resize((static_cast<usize>(n) + 63) / 64);
    used.fill(0);
    ext.clear();
    free_ext.clear();
    head.clear();
    tail.clear();
    total.clear();
    free = n;
}

void DiskLedger::body_slot(u32 body, usize nbodies) {
    if (head.size() < nbodies) {
        const usize old = head.size();
        head.resize(nbodies);
        tail.resize(nbodies);
        total.resize(nbodies);
        for (usize i = old; i < nbodies; ++i)
            head[i] = tail[i] = kNone;
    }
    head[body] = tail[body] = kNone;
    total[body] = 0;
}

u32 DiskLedger::find_run(u32 from, u32 len) const {
    u32 at = free_run_fwd(used.data(), size(), from, len);
    if (at == kNone && from > 0)
        at = free_run_fwd(used.data(), size(), 0, len);
    return at;
}

u32 DiskLedger::add_extent(u32 body, u32 start, u32 len) {
    u32 x;
    if (!free_ext.empty()) {
        x = free_ext.back();
        free_ext.pop();
    } else {
        x = static_cast<u32>(ext.size());
        ext.push(DiskExtent{});
    }
    ext[x] = DiskExtent{start, len, body, kNone};
    for (u32 a = start; a < start + len; ++a) {
        owner[a] = x;
        used[a >> 6] |= 1ull << (a & 63);
    }
    if (tail[body] == kNone)
        head[body] = x;
    else
        ext[tail[body]].next = x;
    tail[body] = x;
    total[body] += len;
    free -= len;
    return x;
}

u32 DiskLedger::alloc_run(Rng& rng, u32 body, u32 len) {
    if (len == 0 || len > free)
        return kNone;
    const u32 at = find_run(static_cast<u32>(rng.below(size())), len);
    if (at == kNone)
        return kNone;
    const u32 off = total[body];
    add_extent(body, at, len);
    return off;
}

u32 DiskLedger::claim_any(Rng& rng, u32 body, u32 count) {
    const u32 n = size();
    if (count == 0 || free == 0 || n == 0)
        return 0;
    u32 need = count, a = static_cast<u32>(rng.below(n)), scanned = 0;
    while (need > 0 && scanned < n) {
        if ((used[a >> 6] >> (a & 63)) & 1) {
            ++a;
            ++scanned;
        } else {
            const u32 st = a;
            u32 len = 0;
            while (len < need && a < n && !((used[a >> 6] >> (a & 63)) & 1)) {
                ++len;
                ++a;
            }
            add_extent(body, st, len);
            need -= len;
            scanned += len;
        }
        if (a == n)
            a = 0;
    }
    return count - need;
}

// Detaches the body's newest k codons (splitting one extent if needed) and
// returns the head of the detached chain, in claim order.
u32 DiskLedger::split_tail(u32 body, u32 k) {
    if (k == 0 || total[body] == 0)
        return kNone;
    if (k >= total[body]) {
        const u32 h = head[body];
        head[body] = tail[body] = kNone;
        total[body] = 0;
        return h;
    }
    const u32 keep = total[body] - k;
    u32 cum = 0, x = head[body];
    while (cum + ext[x].len < keep) {
        cum += ext[x].len;
        x = ext[x].next;
    }
    u32 chain;
    if (cum + ext[x].len == keep) { // the split falls between extents
        chain = ext[x].next;
    } else { // split x: its first part stays
        const u32 stay = keep - cum;
        u32 y;
        if (!free_ext.empty()) {
            y = free_ext.back();
            free_ext.pop();
        } else {
            y = static_cast<u32>(ext.size());
            ext.push(DiskExtent{});
        }
        DiskExtent& X = ext[x];
        ext[y] = DiskExtent{X.start + stay, X.len - stay, body, X.next};
        X.len = stay;
        for (u32 a = ext[y].start; a < ext[y].start + ext[y].len; ++a)
            owner[a] = y;
        chain = y;
    }
    ext[x].next = kNone;
    tail[body] = x;
    total[body] = keep;
    return chain;
}

void DiskLedger::release_tail(u32 body, u32 k, u32 st) {
    u32 x = split_tail(body, k);
    while (x != kNone) {
        DiskExtent& X = ext[x];
        for (u32 a = X.start; a < X.start + X.len; ++a) { // contents stay: fossils
            owner[a] = kNone;
            used[a >> 6] &= ~(1ull << (a & 63));
            if (!stamp.empty() && stamp[a] == 0)
                stamp[a] = st;
        }
        free += X.len;
        const u32 next = X.next;
        X = DiskExtent{0, 0, kNone, kNone};
        free_ext.push(x);
        x = next;
    }
}

void DiskLedger::move_tail(u32 from, u32 to, u32 k) {
    u32 x = split_tail(from, k);
    if (x == kNone)
        return;
    if (tail[to] == kNone)
        head[to] = x;
    else
        ext[tail[to]].next = x;
    for (; x != kNone; x = ext[x].next) {
        ext[x].body = to;
        total[to] += ext[x].len;
        tail[to] = x;
    }
}

void DiskLedger::move_all(u32 from, u32 to) { move_tail(from, to, total[from]); }

bool DiskLedger::addresses(u32 body, u32 off, u32 n, Array<u32>* out) const {
    out->clear();
    if (n == 0 || static_cast<u64>(off) + n > total[body])
        return false;
    u32 x = head[body];
    while (off >= ext[x].len) {
        off -= ext[x].len;
        x = ext[x].next;
    }
    while (out->size() < n) {
        out->push(ext[x].start + off);
        if (++off == ext[x].len) {
            off = 0;
            x = ext[x].next;
            if (x == kNone)
                break;
        }
    }
    return out->size() == n;
}

// Slow bit rot over the whole disk, owned or not, sampled like memory's.
u32 DiskLedger::rot(Rng& rng, double rate) {
    u32 flips = 0;
    const double n = static_cast<double>(size());
    double pos = -log(1.0 - rng.unit()) / rate;
    while (pos < n) {
        mem[static_cast<u32>(pos)] ^= static_cast<u8>(1u << rng.below(6));
        ++flips;
        pos += -log(1.0 - rng.unit()) / rate;
    }
    return flips;
}

void DiskLedger::hash_into(Hasher& h) const {
    h.word(free);
    h.bytes(mem.data(), mem.size());
    h.bytes(owner.data(), owner.size() * sizeof(u32));
    h.bytes(ext.data(), ext.size() * sizeof(DiskExtent));
    h.bytes(free_ext.data(), free_ext.size() * sizeof(u32));
    h.bytes(head.data(), head.size() * sizeof(u32));
    h.bytes(tail.data(), tail.size() * sizeof(u32));
    h.bytes(total.data(), total.size() * sizeof(u32));
    h.bytes(stamp.data(), stamp.size() * sizeof(u32)); // empty without salvage
}

bool DiskLedger::check(const Array<Body>& bodies, TextBuf* why) const {
    auto fail = [why](const char* msg, u64 at) {
        why->append("disk ");
        why->append(msg);
        why->append(" (");
        why->append_u64(at);
        why->append_char(')');
        return false;
    };
    if (head.size() != bodies.size() || total.size() != bodies.size())
        return fail("per-body arrays the wrong size", head.size());
    u64 held = 0;
    for (u32 b = 0; b < bodies.size(); ++b) {
        if (!bodies[b].alive) {
            if (total[b] != 0 || head[b] != kNone)
                return fail("held by a dead body", b);
            continue;
        }
        u64 sum = 0;
        u32 last = kNone, steps = 0;
        for (u32 x = head[b]; x != kNone; x = ext[x].next) {
            if (x >= ext.size() || ++steps > ext.size())
                return fail("extent list out of range or cyclic at body", b);
            if (ext[x].body != b)
                return fail("extent list crosses bodies at extent", x);
            for (u32 a = ext[x].start; a < ext[x].start + ext[x].len; ++a)
                if (a >= size() || owner[a] != x || !((used[a >> 6] >> (a & 63)) & 1))
                    return fail("extent ownership wrong at codon", a);
            sum += ext[x].len;
            last = x;
        }
        if (sum != total[b] || last != tail[b])
            return fail("body disk total or tail wrong at body", b);
        held += sum;
    }
    if (held + free != size())
        return fail("codons not conserved: held", held);
    for (u32 a = 0; a < size(); ++a)
        if ((owner[a] == kNone) == static_cast<bool>((used[a >> 6] >> (a & 63)) & 1))
            return fail("bitmap disagrees at codon", a);
    return true;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void Soup::set_observer(Lineages* obs) {
    obs_ = obs;
    if (obs_)
        obs_->reset(blocks_.size(), p_->size);
}

void Soup::begin_tick(u64 tick) {
    rng_ = Rng(seed_, node_, tick);
    tick_ = tick;
    tally_ = {};
    clear_departures(); // the world took last tick's at the barrier
    if (!emit_.empty()) { // and spread last tick's emissions
        emit_.fill(0);
        emit_count_.fill(0);
        emit_amp_.fill(0);
    }
    // Hazard countdowns in "exposure" units: each instruction (or copied codon)
    // uses up its body's error multiplier. An Exp(1)/rate draw makes the events
    // a Poisson process whose rate follows heat exactly; redrawing every tick is
    // exact too (memorylessness), so no countdown state crosses ticks.
    // The lifecycle's decline (Phase 4): as the node wears, every hazard rate
    // rises by 1 + decline x wear^2. Exactly 1 when the lifecycle is off.
    hazard_mult_ = 1.0;
    if (p_->endurance > 0) {
        double w = static_cast<double>(writes_) / p_->endurance;
        w = w > 1 ? 1 : w;
        hazard_mult_ = 1.0 + p_->decline * w * w;
    }
    flaw_left_ = p_->flaw_rate > 0 ? -log(1.0 - rng_.unit()) / (p_->flaw_rate * hazard_mult_) : 1e300;
    copy_left_ = p_->copy_rate > 0 ? -log(1.0 - rng_.unit()) / (p_->copy_rate * hazard_mult_) : 1e300;
}

// The end of a tick (Phase 4 lifecycle): the starvation meter, and whether the
// node is due to collapse -- worn out, or starved too long. The meter works
// like heat (Damian): it loses node.starve_ebb of itself every tick and gains 1
// on a tick when the free CPU pool is below the threshold, so a brief recovery
// only dents it, and only sustained starvation fills it.
void Soup::lifecycle_check() {
    if (p_->starve_ticks > 0) {
        const bool starving = static_cast<double>(pool_.size()) < p_->starve_below * p_->millicores;
        starve_meter_ = starve_meter_ * (1.0 - p_->starve_ebb) + (starving ? 1.0 : 0.0);
    }
    collapse_ = (p_->endurance > 0 && static_cast<double>(writes_) >= p_->endurance) ||
                (p_->starve_ticks > 0 && starve_meter_ >= p_->starve_ticks);
}

u32 Soup::health() const {
    double h = 1.0;
    if (p_->endurance > 0) {
        const double w = static_cast<double>(writes_) / p_->endurance;
        h = min_of(h, 1.0 - w);
    }
    if (p_->starve_ticks > 0)
        h = min_of(h, 1.0 - starve_meter_ / p_->starve_ticks);
    h = h < 0 ? 0 : h;
    return static_cast<u32>(h * 100.0);
}

// Every body dies an ordinary death: its code stays behind as remnants
// (stamped, with salvage on) and its disk as fossils; units go to the pools.
void Soup::collapse() {
    for (usize k = 0; k < order_.size(); ++k)
        if (bodies_[order_[k]].alive)
            kill(order_[k]);
    for (usize k = 0; k < order_.size(); ++k)
        free_bodies_.push(order_[k]);
    order_.clear();
    clear_departures();
    collapse_ = false;
}

// --- asynchronous accelerator ops (Phase 5, design.md 6.11) -------------------

// The barrier: this tick's issued ops read their inputs from memory as it is
// now (the salvage rule applying, from a stream keyed by the op), and go to
// the world's pending batch, in issue order, with their write-back addresses.
void Soup::accel_gather(Array<AccelOp>* ops, Array<u8>* in, Array<u32>* addr) {
    for (usize i = 0; i < issued_.size(); ++i) {
        AccelOp op = issued_[i];
        const u32* a = issue_addr_.data() + issue_ref_[i];
        const u32 reads = op.src_n + op.dst_n + op.mat_n;
        op.in_off = static_cast<u32>(in->size());
        Rng sr(seed_ ^ 0x73616C7661676500ull, node_, op.serial); // "salvage"
        const double lim = p_->salvage_ratio * op.genome;
        for (u32 j = 0; j < reads; ++j) {
            u8 v = mem_[a[j]];
            if (!stamp_.empty()) {
                const u32 st = stamp_[a[j]];
                if (st != 0 && st > lim && sr.unit() >= lim / st) {
                    v = static_cast<u8>(sr.next() & 63);
                    ++tally_.garbled;
                }
            }
            in->push(v);
        }
        op.addr_off = static_cast<u32>(addr->size());
        addr->append(a + reads, op.out_n);
        ops->push(op);
    }
    issued_.clear();
    issue_ref_.clear();
    issue_addr_.clear();
}

// An op's results: its scalar to the issuing CPU's slot (if that slot still
// holds the same CPU), its codons into the addresses it named, where the
// issuing body still owns them (others are skipped). Hot pages refuse writes
// with their speed factor as probability, and copies make copy errors at the
// GPU's heat, from a stream keyed by the op.
void Soup::accel_complete(const AccelOp& op, const u8* out, u32 scalar, const u32* addr) {
    if (op.cpu < cpu_gen_.size() && cpu_gen_[op.cpu] == op.cpu_gen) {
        if (accel_has_scalar(op.kind))
            cpu_result_[op.cpu] = scalar;
        if (cpu_pending_[op.cpu])
            --cpu_pending_[op.cpu];
    }
    if (op.out_n == 0)
        return;
    const bool alive = op.body < bodies_.size() && bodies_[op.body].alive && bodies_[op.body].serial == op.body_serial;
    if (!alive)
        return;
    Rng r(seed_ ^ 0x6163636F6D700000ull, node_, op.serial); // "accomp"
    const bool copy = op.kind == kAccCopy || op.kind == kAccRcopy;
    const double copy_p = p_->copy_rate * hazard_mult_ * op.gerr;
    for (u32 i = 0; i < op.out_n; ++i) {
        const u32 a = addr[i];
        if (a >= p_->size || owner_[a] == kNone || blocks_[owner_[a]].body != op.body)
            continue;
        if (p_->heat_on) {
            const float h = page_heat_[a >> kPageBits];
            if (h > p_->mem.hot && r.unit() >= p_->mem.speed(h))
                continue; // the page refused the write
        }
        u8 v = out[i];
        if (copy && copy_p > 0 && r.unit() < copy_p) {
            v ^= static_cast<u8>(1u << r.below(6));
            ++tally_.copy_errors;
        }
        mem_[a] = v;
        if (p_->heat_on)
            ++page_write_[a >> kPageBits];
        if (!stamp_.empty())
            stamp_[a] = 0;
        if (p_->endurance > 0)
            ++writes_;
        if (obs_) {
            if (accel_is_npu(op.kind))
                obs_->on_write(a);
            else
                obs_->on_write_gpu(a);
        }
    }
}

void Soup::take_ruins(Ruins* out, u64 fallow) const {
    out->mem.assign(mem_.data(), mem_.size());
    out->mem_stamp.assign(stamp_.data(), stamp_.size());
    out->disk.assign(disk_.mem.data(), disk_.mem.size());
    out->disk_stamp.assign(disk_.stamp.data(), disk_.stamp.size());
    out->fallow = fallow;
}

void Soup::tick(u64 tick) {
    tick_first(tick);
    // Warps (Phase 8): on the CPU reference, between the halves, exactly as
    // the world does it with the GPU.
    if (p_->warps) {
        for (u32 r = 0; r < p_->warp_rounds; ++r) {
            if (!warp_pack(r))
                break;
            warp_run_ref();
            warp_unpack();
            warp_host(r);
        }
    }
    tick_second();
}

// The tick's first half: sparks and the bodies' CPUs.
void Soup::tick_first(u64 tick) {
    begin_tick(tick);

    double rate = p_->spark_rate;
    u32 count = static_cast<u32>(rate);
    if (rng_.chance(rate - count))
        ++count;
    // spark.timing (D10): "first" (Phase 1a) lets chemistry claim freed CPU
    // before life does; "last" leaves it only what the bodies did not claim.
    if (!p_->spark_late)
        sparks(count);

    prepare_heat();
    prepare_gpu();

    // Fixed cyclic order (creation order) with a start rotated by the node RNG,
    // so no body has a permanent first-mover edge (7.1 D). Bodies born during
    // the tick are appended and first run next tick.
    const usize n = order_.size();
    sched_n_ = n;
    sched_start_ = 0;
    if (n > 0) {
        usize start = static_cast<usize>(rng_.below(n));
        sched_start_ = start;
        for (usize k = 0; k < n; ++k) {
            u32 b = order_[(start + k) % n];
            if (bodies_[b].alive)
                run_body(b, tick);
        }
    }
    if (p_->spark_late)
        sparks(count);
}

// The tick's second half: heat, hazards and the death sweep.
void Soup::tick_second() {
    heat_update();
    accel_heat_update();
    bit_rot();
    cpu_wear();
    accel_wear();
    // Phase 4 hazards, after every earlier draw so worlds without them are
    // unchanged.
    if (p_->disk && p_->disk_rot > 0)
        tally_.disk_rot += disk_.rot(rng_, p_->disk_rot);
    if (p_->volatility > 0)
        volatility();
    death_sweep();
    if (!actions_.empty()) // Phase 9
        sweep_actions();
    if (p_->endurance > 0 || p_->starve_ticks > 0)
        lifecycle_check();
}

// --- actions in progress (Phase 9, design.md 6.15) ----------------------------

bool Soup::action_live(const Action& a) const {
    const Body& B = bodies_[a.actor];
    if (!B.alive || B.serial != a.actor_serial)
        return false;
    if (a.warp) // Phase 9b: a warp's, while the warp is still the actor's
        return a.cpu < warps_.size() && warps_[a.cpu].body == a.actor;
    return a.cpu < cpus_.size() && cpus_[a.cpu].body == a.actor;
}

bool Soup::batch_open(Array<u64>& batch, u32 body) {
    while (batch.size() <= body)
        batch.push(0);
    const bool open = batch[body] == tick_ + 1;
    batch[body] = tick_ + 1;
    return open;
}

u32 Soup::action_on(u32 b) const {
    u32 best = kNone;
    for (u32 i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        if (a.target == b && a.target_serial == bodies_[b].serial && action_live(a) &&
            (best == kNone || a.end < actions_[best].end))
            best = i;
    }
    return best;
}

u32 Soup::action_of(u32 c) const {
    for (u32 i = 0; i < actions_.size(); ++i)
        if (!actions_[i].warp && actions_[i].cpu == c && actions_[i].actor == cpus_[c].body &&
            action_live(actions_[i]))
            return i;
    return kNone;
}

// After the death sweep: an action whose actor died or was eaten is lost
// (its CPU, if it lives on in an eater, is free again).
void Soup::sweep_actions() {
    usize w = 0;
    for (usize i = 0; i < actions_.size(); ++i) {
        if (action_live(actions_[i]))
            actions_[w++] = actions_[i];
        else
            ++tally_.actions_failed;
    }
    actions_.resize(w);
}

// Volatility (Phase 4, design.md 6.10; 5.1 A): a wipe starts at each page with
// chance mem.volatility a tick (a Poisson process over the pages) and covers
// 1..mem.wipe_pages whole pages, which become random codons. Ownership stays:
// the body still holds that memory, now garbage, and can repair it from disk.
// A body all of whose owned memory was wiped this tick has its CPUs halted,
// so it dies in the sweep: a whole-body hit is death.
void Soup::volatility() {
    const u32 pages = static_cast<u32>(page_heat_.size());
    const double rate = p_->volatility * hazard_mult_;
    double pos = -log(1.0 - rng_.unit()) / rate;
    if (pos >= pages)
        return;
    wiped_pg_.resize(pages);
    wiped_pg_.fill(0);
    wiped_bodies_.clear();
    while (pos < pages) {
        const u32 pg = static_cast<u32>(pos);
        const u32 len = 1 + static_cast<u32>(rng_.below(p_->wipe_pages));
        const u32 start = pg << kPageBits;
        const u32 end = static_cast<u32>(min_of<u64>(static_cast<u64>(pg + len) << kPageBits, p_->size));
        for (u32 a = start; a < end; ++a) {
            mem_[a] = static_cast<u8>(rng_.next() & 63);
            if (!stamp_.empty())
                stamp_[a] = 0; // random codons are nobody's code
            if (owner_[a] != kNone)
                wiped_bodies_.push(blocks_[owner_[a]].body);
        }
        for (u32 q = pg; q < pages && (q << kPageBits) < end; ++q)
            wiped_pg_[q] = 1;
        if (obs_)
            obs_->on_wipe(start, end - start);
        ++tally_.wipes;
        tally_.wiped += end - start;
        pos += -log(1.0 - rng_.unit()) / rate;
    }
    // Whole-body hits, each body once, in slot order.
    Array<u64> ids;
    for (usize i = 0; i < wiped_bodies_.size(); ++i)
        ids.push(wiped_bodies_[i]);
    sort_u64(ids.data(), ids.size());
    for (usize i = 0; i < ids.size(); ++i) {
        if (i > 0 && ids[i] == ids[i - 1])
            continue;
        const u32 b = static_cast<u32>(ids[i]);
        Body& B = bodies_[b];
        if (!B.alive)
            continue;
        bool whole = true;
        for (u32 k = B.blocks; k != kNone && whole; k = blocks_[k].next) {
            const Block& bl = blocks_[k];
            for (u32 a = bl.start; a < bl.start + bl.len && whole; ++a)
                if (owner_[a] == k && !wiped_pg_[a >> kPageBits])
                    whole = false;
        }
        if (!whole)
            continue;
        for (u32 c = B.cpus; c != kNone; c = cpus_[c].next)
            cpus_[c].halted = 1;
        if (p_->warps) // and its warps end (Phase 8)
            kill_warps(b);
        ++tally_.wipe_deaths;
    }
}

void Soup::sparks(u32 count) {
    // Live knob changes may leave min above max for a while; use the range.
    const u32 lo = min_of(p_->spark_min, p_->spark_max);
    const u32 hi = max_of(p_->spark_min, p_->spark_max);
    for (u32 i = 0; i < count; ++i) {
        // Phase 8: with warps a spark may draw GPU units instead (or as well).
        const bool gpu_pool = p_->warps && p_->spark_gpu_grant && p_->gpus && !gpu_.pool.empty();
        if (pool_.empty() && !gpu_pool)
            return; // a spark only draws from the free pool
        u32 len = lo + static_cast<u32>(rng_.below(hi - lo + 1));
        u32 from = static_cast<u32>(rng_.below(p_->size));
        u32 at = known_full(len) ? kNone : find_free_fwd(from, len);
        if (at == kNone && !known_full(len))
            at = find_free_fwd(0, len);
        if (at == kNone) {
            note_full(len);
            continue;
        }
        if (!p_->warps) {
            create_body(at, len, p_->spark_grant);
        } else {
            // Its form: whichever it holds; holding both, the GPU with
            // chance spark.gpu_born.
            const u32 b = new_body();
            new_block(at, len, b);
            claim_units(b, p_->spark_grant);
            if (p_->spark_gpu_grant)
                claim_gpus(b, p_->spark_gpu_grant);
            u32 form = kFormCpu;
            if (bodies_[b].units == 0)
                form = kFormGpu;
            else if (gpu_count(b) > 0 && rng_.chance(p_->spark_gpu_born))
                form = kFormGpu;
            birth(b, at, form);
        }
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
    // Memory upkeep (Phase 1b D1): before it runs, a body pays
    // u * m * (m / 64)^(e - 1) instructions for the m codons it owns. With
    // e = 4/3 (the square-cube law) the cost per codon grows with size, so
    // growth costs more the bigger a body gets. It is charged as work, so it
    // heats the body, and whatever the budget cannot cover is carried as debt.
    double upkeep = 0;
    // Phase 8: a body with a running warp but no running CPU pays its upkeep
    // from its warps, at the serial rate (warp_pack); with both, its warps
    // pay whatever its CPUs can't (below).
    const bool warps = p_->warps && warp_runs(b);
    if (warps && !cpu_runs(b)) {
        if (p_->upkeep > 0 && bodies_[b].mem > 0) {
            const double m = bodies_[b].mem;
            upkeep = p_->upkeep * m;
            if (p_->upkeep_exp != 1.0)
                upkeep *= pow(m / 64.0, p_->upkeep_exp - 1.0);
            warp_upkeep_[b] += upkeep;
            tally_.upkeep += upkeep < 4e9 ? static_cast<u64>(upkeep) : 4000000000ull;
        }
        bodies_[b].carry = 0.0f; // nothing to run on its CPUs
        return;
    }
    if (p_->upkeep > 0 && bodies_[b].mem > 0) {
        const double m = bodies_[b].mem;
        upkeep = p_->upkeep * m;
        if (p_->upkeep_exp != 1.0)
            upkeep *= pow(m / 64.0, p_->upkeep_exp - 1.0);
        const u64 charged = upkeep < 4e9 ? static_cast<u64>(upkeep) : 4000000000ull;
        // Phase 9: upkeep is that many plain instructions' time (classic: x 1)
        const double time = p_->cost.timed ? static_cast<double>(charged) * p_->plain : static_cast<double>(charged);
        const u64 work = bodies_[b].work + (time < 4e9 ? static_cast<u64>(time) : 4000000000ull);
        bodies_[b].work = work > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(work);
        tally_.upkeep += charged;
        if (p_->cost.timed)
            upkeep *= p_->plain;
    }
    // The CPU time a tick (Phase 9: tick_units is instr_per_core, or that many
    // plain instructions' picoseconds with a timed table).
    double budget = static_cast<double>(bodies_[b].speed) * p_->tick_units / 1000.0 +
                    bodies_[b].carry - upkeep;
    if (warps && budget < 0) { // Phase 8: the shortfall goes to its warps instead of debt
        warp_upkeep_[b] += p_->cost.timed ? -budget / p_->plain : -budget; // (warps count plain instructions)
        budget = 0;
    }
    i64 n = budget > 0 ? static_cast<i64>(floor(budget)) : 0;
    bodies_[b].carry = static_cast<float>(budget - static_cast<double>(n));
    if (n <= 0) {
        // Phase 9: with a timed table one instruction can run far past its
        // tick (a kernel call is ~2,000 adds). A tick spent paying that debt
        // is a tick the CPU spent on it: busy, as heat sees it (heat_update
        // counts at most a full tick of work in the tick that ran over).
        if (p_->cost.timed) {
            const double cap = static_cast<double>(bodies_[b].speed) * p_->tick_units / 1000.0 - upkeep;
            if (cap > 0) {
                const u64 work = bodies_[b].work + (cap < 4e9 ? static_cast<u64>(cap) : 4000000000ull);
                bodies_[b].work = work > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(work);
            }
        }
        return;
    }

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
        float nominal = static_cast<float>(B.units) * static_cast<float>(p_->tick_units) / 1000.0f;
        float work = static_cast<float>(B.work);
        if (p_->cost.timed && work > nominal) // Phase 9: a tick's overshoot heats the ticks that pay it off
            work = nominal;
        B.speed = nominal > 0 ? p_->cpu_gain * work / nominal : 0.0f;
    }
    const u32 m = p_->millicores;
    // The ambient leak (Phase 4, design.md 6.10): heat.ambient x the mean CPU
    // heat input over every millicore, held or free, reaches every unit and
    // page on the node, so one busy body warms everyone a little.
    ambient_ = 0.0f;
    if (p_->ambient > 0 && m > 0) {
        double sum = 0;
        for (usize k = 0; k < order_.size(); ++k) {
            const Body& B = bodies_[order_[k]];
            sum += static_cast<double>(B.speed) * B.units;
        }
        ambient_ = static_cast<float>(p_->ambient * sum / m);
    }
    for (u32 u = 0; u < m; ++u) {
        u32 o = unit_owner_[u];
        float h = unit_heat_[u] * decay + (o != kNone ? bodies_[o].speed : 0.0f);
        if (ambient_ > 0)
            h += ambient_;
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
        if (ambient_ > 0)
            h += ambient_;
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
                if (!stamp_.empty() && stamp_[a] == 0)
                    stamp_[a] = genome_len(blocks_[k].body);
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
    const double rate = p_->rot_rate * top * hazard_mult_;
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

// CPU wear (Phase 1b D10): each held millicore fails back into the pool,
// keeping its heat, with chance cpu.wear per tick -- "nothing grants
// permanence" applied to CPU, which otherwise costs nothing to hold. Sampled as
// a Poisson process over the unit index, like bit rot.
void Soup::cpu_wear() {
    const double f = p_->cpu_wear;
    if (f <= 0)
        return;
    const double m = static_cast<double>(p_->millicores);
    double pos = -log(1.0 - rng_.unit()) / f;
    while (pos < m) {
        u32 u = static_cast<u32>(pos);
        if (unit_owner_[u] != kNone) {
            unlink_unit(u);
            pool_push(u);
            ++tally_.cpu_worn;
        }
        pos += -log(1.0 - rng_.unit()) / f;
    }
}

void Soup::death_sweep() {
    // A body dies when it has no running CPU: no un-halted CPU, or no
    // millicore to run on (6.4 #1). Engulfed bodies are already gone.
    // Phase 8: ... and no running warp. A warp whose lanes have all halted
    // has ended, and goes first.
    for (usize k = 0; k < order_.size(); ++k) {
        u32 b = order_[k];
        Body& B = bodies_[b];
        if (!B.alive)
            continue;
        bool running = false;
        if (B.units > 0)
            for (u32 c = B.cpus; c != kNone && !running; c = cpus_[c].next)
                running = !cpus_[c].halted;
        if (p_->warps) {
            for (u32 w = warp_head_[b]; w != kNone;) {
                const u32 next = warps_[w].next;
                if (wlive_[w] == 0)
                    free_warp(w);
                w = next;
            }
            running = running || warp_runs(b);
        }
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
    if (p_->gpus) // without a GPU (NPU) the hash is exactly as before them
        gpu_.hash_into(h);
    if (p_->npus)
        npu_.hash_into(h);
    if (p_->disk) // Phase 4, likewise
        disk_.hash_into(h);
    if (!stamp_.empty())
        h.bytes(stamp_.data(), stamp_.size() * sizeof(u32));
    if (p_->accel_async) { // Phase 5: issue count and the CPUs' slots
        h.word(accel_serial_);
        h.bytes(cpu_gen_.data(), cpu_gen_.size() * sizeof(u32));
        h.bytes(cpu_result_.data(), cpu_result_.size() * sizeof(u32));
        h.bytes(cpu_pending_.data(), cpu_pending_.size() * sizeof(u32));
    }
    if (lifecycle()) {
        h.word(writes_);
        u64 bits;
        memcpy(&bits, &starve_meter_, sizeof bits);
        h.word(bits);
    }
    if (p_->warps) { // Phase 8: warps and their lanes (the world fetches any the GPU holds first)
        NOVA_ASSERT(!lanes_on_device());
        h.bytes(warps_.data(), warps_.size() * sizeof(Warp));
        h.bytes(free_warps_.data(), free_warps_.size() * sizeof(u32));
        h.bytes(lanes_.data(), lanes_.size() * sizeof(Lane));
        h.bytes(warp_head_.data(), warp_head_.size() * sizeof(u32));
        h.bytes(warp_count_.data(), warp_count_.size() * sizeof(u32));
        h.bytes(warp_carry_.data(), warp_carry_.size() * sizeof(float));
        if (p_->cost.timed) // Phase 9b: when each warp may run again
            h.bytes(wuntil_.data(), wuntil_.size() * sizeof(u64));
    }
    if (p_->cost.timed) { // Phase 9: actions in progress (classic: exactly as before)
        h.word(actions_.size());
        h.bytes(actions_.data(), actions_.size() * sizeof(Action));
    }
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

// The same for the disk tier's per-codon arrays (in RAM or in files, Phase 7b).
template <class T> void put_array(ByteWriter& w, const Backed<T>& a) {
    w.put_u32(static_cast<u32>(a.size()));
    w.put_bytes(a.data(), a.size() * sizeof(T));
}

template <class T> bool get_array(ByteReader& r, Backed<T>* a, u64 max_count) {
    u32 n = 0;
    if (!r.get_u32(&n) || n > max_count || static_cast<u64>(n) * sizeof(T) > r.remaining())
        return false;
    a->resize(n);
    return r.get_bytes(a->data(), static_cast<usize>(n) * sizeof(T));
}

} // namespace

void UnitLedger::save(ByteWriter& w) const {
    put_array(w, owner);
    put_array(w, heat);
    put_array(w, next);
    put_array(w, prev);
    put_array(w, pool);
    put_array(w, head);
    put_array(w, tail);
    put_array(w, count);
}

void DiskLedger::save(ByteWriter& w) const {
    w.put_u32(free);
    put_array(w, mem);
    put_array(w, owner);
    put_array(w, ext);
    put_array(w, free_ext);
    put_array(w, head);
    put_array(w, tail);
    put_array(w, total);
    if (!stamp.empty())
        put_array(w, stamp);
}

bool DiskLedger::load(ByteReader& r, u64 nbodies, TextBuf* why) {
    const u64 n = size(), many = 0xFFFFFFFFull;
    const bool stamped = !stamp.empty(); // sized at init when salvage is on
    bool ok = r.get_u32(&free) && get_array(r, &mem, n) && get_array(r, &owner, n) && get_array(r, &ext, many) &&
              get_array(r, &free_ext, many) && get_array(r, &head, nbodies) && get_array(r, &tail, nbodies) &&
              get_array(r, &total, nbodies) && (!stamped || get_array(r, &stamp, n));
    if (!ok || mem.size() != n || owner.size() != n || head.size() != nbodies || tail.size() != nbodies ||
        total.size() != nbodies || (stamped && stamp.size() != n)) {
        why->append("snapshot: disk section truncated or the wrong shape");
        return false;
    }
    used.resize((static_cast<usize>(n) + 63) / 64);
    used.fill(0);
    for (u32 a = 0; a < n; ++a) {
        if (mem[a] > 63 || (owner[a] != kNone && owner[a] >= ext.size())) {
            why->append("snapshot: disk contents or ownership out of range");
            return false;
        }
        if (owner[a] != kNone)
            used[a >> 6] |= 1ull << (a & 63);
    }
    for (usize i = 0; i < ext.size(); ++i) {
        const DiskExtent& X = ext[i];
        if (X.body != kNone && (static_cast<u64>(X.start) + X.len > n || X.body >= nbodies ||
                                (X.next != kNone && X.next >= ext.size()))) {
            why->append("snapshot: disk extent out of range");
            return false;
        }
    }
    return true;
}

bool UnitLedger::load(ByteReader& r, u64 nbodies, TextBuf* why) {
    const u64 n = units();
    bool ok = get_array(r, &owner, n) && get_array(r, &heat, n) && get_array(r, &next, n) &&
              get_array(r, &prev, n) && get_array(r, &pool, n) && get_array(r, &head, nbodies) &&
              get_array(r, &tail, nbodies) && get_array(r, &count, nbodies);
    if (!ok || owner.size() != n || heat.size() != n || next.size() != n || prev.size() != n ||
        head.size() != nbodies || tail.size() != nbodies || count.size() != nbodies) {
        why->append("snapshot: unit section truncated or the wrong shape");
        return false;
    }
    pool_pos.resize(n);
    pool_pos.fill(kNone);
    for (u32 i = 0; i < pool.size(); ++i) {
        if (pool[i] >= n) {
            why->append("snapshot: unit pool out of range");
            return false;
        }
        pool_pos[pool[i]] = i;
    }
    cap.resize(nbodies);
    cap.fill(0.0f);
    done.resize(nbodies);
    done.fill(0);
    err.resize(nbodies);
    err.fill(1.0f);
    return true;
}

bool UnitLedger::check(const Array<Body>& bodies, const char* what, TextBuf* why) const {
    auto fail = [why, what](const char* msg, u64 at) {
        why->append(what);
        why->append_char(' ');
        why->append(msg);
        why->append(" (");
        why->append_u64(at);
        why->append_char(')');
        return false;
    };
    const u32 n = units();
    if (head.size() != bodies.size() || count.size() != bodies.size())
        return fail("per-body arrays the wrong size", head.size());
    u64 held = 0;
    for (u32 b = 0; b < bodies.size(); ++b) {
        if (!bodies[b].alive) {
            if (count[b] != 0 || head[b] != kNone)
                return fail("units held by a dead body", b);
            continue;
        }
        u32 c = 0;
        for (u32 u = head[b]; u != kNone; u = next[u]) {
            if (u >= n || c > n)
                return fail("unit list out of range or cyclic at body", b);
            if (owner[u] != b)
                return fail("unit list crosses bodies at unit", u);
            ++c;
        }
        if (c != count[b])
            return fail("body unit count wrong at body", b);
        held += c;
    }
    if (held + pool.size() != n)
        return fail("units not conserved: held", held);
    for (u32 i = 0; i < pool.size(); ++i)
        if (owner[pool[i]] != kNone || pool_pos[pool[i]] != i)
            return fail("pool entry inconsistent at", i);
    return true;
}

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
    if (p_->gpus) // the embedded config says whether these sections exist
        gpu_.save(w);
    if (p_->npus)
        npu_.save(w);
    if (p_->disk)
        disk_.save(w);
    if (p_->salvage_ratio > 0)
        put_array(w, stamp_);
    if (p_->accel_async) {
        w.put_u64(accel_serial_);
        put_array(w, cpu_gen_);
        put_array(w, cpu_result_);
        put_array(w, cpu_pending_);
    }
    if (lifecycle()) {
        w.put_u64(writes_);
        u64 bits;
        memcpy(&bits, &starve_meter_, sizeof bits);
        w.put_u64(bits);
    }
    if (p_->warps) {
        NOVA_ASSERT(!lanes_on_device());
        put_array(w, warps_);
        put_array(w, free_warps_);
        put_array(w, lanes_);
        put_array(w, warp_head_);
        put_array(w, warp_count_);
        put_array(w, warp_carry_);
        if (p_->cost.timed) // Phase 9b
            put_array(w, wuntil_);
    }
    if (p_->cost.timed) // Phase 9
        put_array(w, actions_);
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
    const u64 nb = bodies_.size();
    gpu_.init(p_->gpus); // sized from the config; load fills them
    npu_.init(p_->npus);
    if (p_->gpus && !gpu_.load(r, nb, err))
        return false;
    if (p_->npus && !npu_.load(r, nb, err))
        return false;
    if (p_->disk && !disk_.load(r, nb, err))
        return false;
    if (p_->salvage_ratio > 0 && (!get_array(r, &stamp_, n) || stamp_.size() != n)) {
        err->append("snapshot: stamp section truncated or the wrong shape");
        return false;
    }
    if (p_->accel_async) {
        const u64 nc = cpus_.size();
        if (!(r.get_u64(&accel_serial_) && get_array(r, &cpu_gen_, nc) && get_array(r, &cpu_result_, nc) &&
              get_array(r, &cpu_pending_, nc))) {
            err->append("snapshot: accelerator section truncated");
            return false;
        }
        issued_.clear();
        issue_ref_.clear();
        issue_addr_.clear();
    }
    if (lifecycle()) {
        u64 bits = 0;
        if (!(r.get_u64(&writes_) && r.get_u64(&bits))) {
            err->append("snapshot: lifecycle section truncated");
            return false;
        }
        memcpy(&starve_meter_, &bits, sizeof bits);
    }
    if (p_->warps) { // Phase 8
        const u64 lanes_max = many;
        if (!(get_array(r, &warps_, many) && get_array(r, &free_warps_, many) && get_array(r, &lanes_, lanes_max) &&
              get_array(r, &warp_head_, nb) && get_array(r, &warp_count_, nb) && get_array(r, &warp_carry_, nb)) ||
            lanes_.size() != warps_.size() * p_->warp_lanes || warp_head_.size() != nb ||
            warp_count_.size() != nb || warp_carry_.size() != nb) {
            err->append("snapshot: warp section truncated or the wrong shape");
            return false;
        }
        warp_upkeep_.resize(nb);
        warp_upkeep_.fill(0.0);
        wleft_.resize(warps_.size());
        wleft_.fill(0);
        wsteps_.resize(warps_.size());
        wsteps_.fill(0);
        wtime_.resize(warps_.size());
        wtime_.fill(0);
        wagain_.resize(warps_.size());
        wagain_.fill(0);
        wlive_.resize(warps_.size());
        wact_.resize(warps_.size());
        wstate_.resize(warps_.size());
        wstate_.fill(kLanesHost);
        for (u32 w = 0; w < warps_.size(); ++w)
            count_lanes(w);
        wchain_ = 0;
        build_warp_consts();
        wuntil_.resize(warps_.size()); // Phase 9b: when each may run again (timed tables)
        wuntil_.fill(0);
        if (p_->cost.timed && (!get_array(r, &wuntil_, many) || wuntil_.size() != warps_.size())) {
            err->append("snapshot: warp waits truncated or the wrong shape");
            return false;
        }
    }
    gpu_batch_.clear();
    npu_batch_.clear();
    actions_.clear();
    if (p_->cost.timed && !get_array(r, &actions_, many)) { // Phase 9
        err->append("snapshot: actions section truncated");
        return false;
    }
    collapse_ = false; // a snapshot is taken after the barrier applied any collapse
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

    // GPU and NPU units: per-body lists, owners, the pool, conservation.
    if (p_->gpus && !gpu_.check(bodies_, "GPU", why))
        return false;
    if (p_->npus && !npu_.check(bodies_, "NPU", why))
        return false;
    if (p_->disk && !disk_.check(bodies_, why))
        return false;

    // Warps (Phase 8): per-body lists, owners, counts, every slot used or free.
    if (p_->warps) {
        if (warp_head_.size() != bodies_.size() || warp_count_.size() != bodies_.size() ||
            lanes_.size() != warps_.size() * p_->warp_lanes)
            return fail("warp arrays the wrong size", warps_.size());
        usize held = 0;
        for (u32 b = 0; b < bodies_.size(); ++b) {
            if (!bodies_[b].alive) {
                if (warp_count_[b] != 0 || warp_head_[b] != kNone)
                    return fail("warps held by a dead body", b);
                continue;
            }
            u32 c = 0;
            for (u32 w = warp_head_[b]; w != kNone; w = warps_[w].next) {
                if (w >= warps_.size() || c > warps_.size())
                    return fail("warp list out of range or cyclic at body", b);
                if (warps_[w].body != b)
                    return fail("warp list crosses bodies at warp", w);
                if (wstate_[w] != kLanesDevice) { // the counts follow the lanes the host holds
                    u32 running = 0;
                    for (u32 l = 0; l < p_->warp_lanes; ++l)
                        running += lane(w, l).halted ? 0 : 1;
                    if (running != wlive_[w])
                        return fail("warp's running-lane count wrong, warp", w);
                }
                ++c;
            }
            if (c != warp_count_[b] || c > p_->warp_max)
                return fail("body warp count wrong at body", b);
            held += c;
        }
        if (held + free_warps_.size() != warps_.size())
            return fail("warp slots not conserved: held", held);
        for (usize i = 0; i < free_warps_.size(); ++i)
            if (free_warps_[i] >= warps_.size() || warps_[free_warps_[i]].body != kNone)
                return fail("free warp slot in use at", i);
    }
    // Phase 9: actions in progress name slots that exist, by live actors (the
    // sweep drops the rest at the end of every tick), one per CPU.
    if (!actions_.empty() && !p_->cost.timed)
        return fail("actions in progress without a timed cost table", actions_.size());
    for (usize i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        if (a.actor >= bodies_.size() || a.cpu >= (a.warp ? warps_.size() : cpus_.size()) || a.warp > 1 ||
            (a.target != kNone && a.target >= bodies_.size()) || a.n > 64 || a.op >= kOpCount)
            return fail("action out of range at", i);
        for (usize j = 0; j < i; ++j)
            if (actions_[j].cpu == a.cpu && actions_[j].warp == a.warp && action_live(actions_[j]) &&
                action_live(a))
                return fail("two actions on one CPU or warp at", i);
    }
    return true;
}

} // namespace nova
