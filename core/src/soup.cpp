#include "nova/soup.h"

#include <intrin.h> // _BitScanForward64, _BitScanReverse64

#include <cmath>  // floor, log, pow
#include <cstdio> // snprintf

#include "nova/keys.h" // the settings registry
#include "nova/lineage.h"
#include "nova/pool.h"    // clock_seconds (profiling)
#include "nova/profile.h" // behaviour profiles (A3): the sampler's hooks

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

// Phase 10 (design.md 6.16): a codon with one random set bit cleared, as dead
// matter decays toward codon 0 (INERT in nova_v2). v must not be 0.
u8 fade_bit(u8 v, Rng& rng) {
    u32 bits[6], n = 0;
    for (u32 b = 0; b < 6; ++b)
        if (v & (1u << b))
            bits[n++] = b;
    return static_cast<u8>(v & ~(1u << bits[rng.below(n)]));
}

// A unit's heat curve: the heat.<unit>.* family (keys.inc).
HeatCurve get_curve(Config& cfg, const char* prefix) {
    char key[64];
    HeatCurve c = {};
    snprintf(key, sizeof key, "%s.warm", prefix);
    c.warm = static_cast<float>(key_f64(cfg, key));
    snprintf(key, sizeof key, "%s.hot", prefix);
    c.hot = static_cast<float>(key_f64(cfg, key));
    snprintf(key, sizeof key, "%s.max", prefix);
    c.max = static_cast<float>(key_f64(cfg, key));
    snprintf(key, sizeof key, "%s.speed_min", prefix);
    c.speed_min = static_cast<float>(key_f64(cfg, key));
    snprintf(key, sizeof key, "%s.err_max", prefix);
    c.err_max = static_cast<float>(key_f64(cfg, key));
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

void set_warp_lanes(SoupParams* p, u32 lanes) {
    p->warp_lanes = lanes;
    if (p->cost.timed) {
        const double v = static_cast<double>(lanes) * static_cast<double>(p->cost.gpu_thr);
        p->warp_step_cost = v < 1 ? 1u : (v > 4e9 ? 4000000000u : static_cast<u32>(v));
    }
}

u32 draw_warp_lanes(const SoupParams& p, u64 seed, u32 node) {
    u32 widths = 0; // how many powers of two from min to max
    for (u32 w = p.warp_lanes_min; w != 0 && w <= p.warp_lanes_max; w <<= 1)
        ++widths;
    if (widths <= 1)
        return p.warp_lanes_min;
    Rng r(seed ^ 0x7761727077696474ull, node, 0); // "warpwidt": its own stream
    return p.warp_lanes_min << static_cast<u32>(r.below(widths));
}

bool soup_params_from_config(Config& cfg, u32 soup_size, SoupParams* p) {
    p->size = soup_size;
    p->millicores = key_u32(cfg, "world.millicores");
    p->instr_per_core = key_u32(cfg, "cpu.instr_per_core");
    p->halt_after = key_u32(cfg, "cpu.halt_after");

    // The in-world GPU (Phase 2, design.md 6.7). No units = no GPU: every GPU
    // selector fails as in Phase 1, and nothing else changes.
    // The real devices (Phase 5, design.md 6.11): asynchronous, batched ops,
    // with a high-volume profile by default (Damian: GPUs are built for
    // volume, so expensive per op and nearly free per element). Instant mode
    // is exactly Phase 4.
    Str mode = key_choice(cfg, "accel.mode"); // Phase 10: every feature on by default
    p->accel_async = mode.eq("async");
    p->accel_latency = key_u32(cfg, "accel.latency");
    const bool hv = p->accel_async;
    p->gpus = key_u32(cfg, "world.gpus");
    p->gpu_elements = key_u32(cfg, "gpu.elements", key_default(hv ? 4096 : 32));
    p->gpu_setup = key_u32(cfg, "gpu.setup", key_default(hv ? 32 : 4));
    p->gpu_cpu_per_element = key_f64(cfg, "gpu.cpu_per_element", key_default(hv ? 1.0 / 1024.0 : 0.0625));
    p->gpu_wear = key_f64(cfg, "gpu.wear");
    // The in-world NPU (Phase 2b, design.md 6.8), the same way.
    p->npus = key_u32(cfg, "world.npus");
    p->npu_macs = key_u32(cfg, "npu.macs", key_default(hv ? 65536 : 256));
    p->npu_cpu_per_mac = key_f64(cfg, "npu.cpu_per_mac", key_default(hv ? 1.0 / 16384.0 : 1.0 / 64.0));
    p->npu_max_matvec = hv ? 256 : 64;
    p->npu_max_matmul = hv ? 64 : 32;
    p->npu_wear = key_f64(cfg, "npu.wear");

    // Phase 1b rules (design.md 6.6). mem.upkeep = 0 is exactly Phase 1a.
    p->upkeep = key_f64(cfg, "mem.upkeep");
    p->upkeep_exp = key_f64(cfg, "mem.upkeep_exp");
    p->cpu_wear = key_f64(cfg, "cpu.wear");
    p->spark_late = key_choice(cfg, "spark.timing").eq("last");

    Str table = key_choice(cfg, "isa.table");
    if (table.eq("custom")) {
        Str codons = key_str(cfg, "isa.codons");
        TextBuf err;
        if (!parse_table(codons, &p->table, &err))
            cfg.fail("isa.codons", err.c_str());
    } else if (!builtin_table(table, &p->table)) {
        NOVA_FATAL("soup_params_from_config: an isa.table option with no built-in table");
    }

    // Selector numbering (D6): nearest-to-self, or a per-run shuffle drawn from
    // isa.selector_seed (0 = the world seed, so paired runs differ by seed).
    Str sel = key_choice(cfg, "isa.selectors");
    u64 sel_seed = key_u64(cfg, "isa.selector_seed");
    // Dispersal (Phase 3): DIVIDE to a neighbour, on by default only in
    // procedural worlds, so ring worlds stay exactly as before.
    const Str graph = key_choice(cfg, "world.graph");
    const bool spatial = graph.eq("procedural") || graph.eq("landscape"); // Phase 7: landscapes too
    p->dispersal = key_flag(cfg, "world.dispersal", key_default(spatial));
    // The disk tier and volatility (Phase 4): no disk = everything disk fails,
    // as before; no volatility = no wipes.
    p->disk = key_u32(cfg, "world.disk");
    p->disk_cost_base = key_u32(cfg, "disk.cost_base");
    p->disk_cost_per_codon = key_f64(cfg, "disk.cost_per_codon");
    p->disk_rot = key_f64(cfg, "disk.rot", key_default(p->disk ? (p->table.op[0] == Op::inert ? 1e-8 : 1e-6) : 0.0)); // Phase 10: rarer with decay
    // Disk tiers (docs/world-plan.md W4): the archive, a quarter of the disk,
    // rotting at a hundredth of its rate, archive.slow times dearer to use.
    // 0 = no archive: everything as before. (archive.decay: after disk.decay.)
    p->archive = key_u32(cfg, "world.archive", key_default(p->disk / 4));
    p->archive_rot = key_f64(cfg, "archive.rot", key_default(p->disk_rot / 100));
    p->archive_slow = key_f64(cfg, "archive.slow");
    // (the tiers' substrate speeds, disk_speed and archive_speed, are set to 1
    // below with the others; the world sets them per node: W1)
    p->volatility = key_f64(cfg, "mem.volatility");
    p->wipe_pages = key_u32(cfg, "mem.wipe_pages");
    p->wipe_warning = key_u32(cfg, "mem.wipe_warning"); // docs/lifehistory-plan.md W
    p->wipe_false_alarms = key_f64(cfg, "mem.wipe_false_alarms");
    p->wipe_flicker = key_f64(cfg, "mem.wipe_flicker");
    // Memory tiers (docs/world-plan.md W3): the first mem.fast_share of each
    // node's soup is fast memory (the world sets each node's fast_end from its
    // own size); 0 = one tier, everything as before.
    p->mem_fast_share = key_f64(cfg, "mem.fast_share");
    p->fast_volatility = key_f64(cfg, "mem.fast_volatility");
    p->fast_end = mem_fast_end(soup_size, p->mem_fast_share);
    // Remnants and salvage (Phase 4, Damian): 0 = off, every read as before.
    p->salvage_ratio = key_f64(cfg, "salvage.ratio");
    // The signal channel (Phase 4): 0 channels = EMIT and LISTEN fail.
    p->signal_channels = key_u32(cfg, "signal.channels");
    p->signal_cost_base = key_u32(cfg, "signal.cost_base");
    p->signal_cost_per_amp = key_f64(cfg, "signal.cost_per_amp");
    p->signal_floor = key_f64(cfg, "signal.floor");
    // The node lifecycle (Phase 4): endurance 0 = off. Collapse is wear alone
    // (Damian, docs/dormancy-plan.md D7): the starvation rule and its keys
    // (node.starve_below, node.starve_ticks, node.starve_ebb) are gone.
    p->endurance = key_f64(cfg, "node.endurance", key_default(spatial ? 1e9 : 0.0)); // (the lifecycle needs a map)
    p->decline = key_f64(cfg, "node.decline");
    p->map = spatial;

    // Measured costs (Phase 9, design.md 6.15): classic = exactly Phase 8.
    const Str ct = key_choice(cfg, "cost.table");
    if (!builtin_costs(ct, &p->cost))
        NOVA_FATAL("soup_params_from_config: a cost.table option with no built-in table");
    if (!builtin_costs_e(Str(p->cost.name), &p->ecost)) // Phase 9c: on an E-core
        p->ecost = p->cost;
    p->plain = p->cost.timed ? static_cast<double>(p->cost.alu) : 1.0;
    p->tick_units = static_cast<double>(p->instr_per_core) * p->plain;
    p->engulf_cpt = key_u32(cfg, "engulf.codons_per_tick"); // Phase 10: halved, so prey can react
    p->corpse_cost = key_f64(cfg, "engulf.corpse_cost");    // cheaper bites of fresh corpses (2026-11-08)

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
    if (p->disk || p->archive) // (W4: an archive alone is a disk too)
        sense_n = kSenseReadingsDisk;
    if (p->endurance > 0)
        sense_n = kSenseReadingsLife; // + node health
    if (p->accel_async)
        sense_n = kSenseReadingsAsync; // + my last accelerator result, my ops pending

    // Warps (Phase 8, design.md 6.14): off = everything exactly as Phase 7.
    p->warps = key_flag(cfg, "gpu.warps");
    // docs/inject-plan.md W1: each node's width is drawn between these
    // (World::derive_node); a soup outside a world runs the narrowest.
    p->warp_lanes_min = key_u32(cfg, "warp.lanes_min");
    p->warp_lanes_max = key_u32(cfg, "warp.lanes_max");
    auto pow2 = [](u32 v) { return v != 0 && (v & (v - 1)) == 0; };
    if (!pow2(p->warp_lanes_min))
        cfg.fail("warp.lanes_min", "warp widths are powers of two");
    if (!pow2(p->warp_lanes_max))
        cfg.fail("warp.lanes_max", "warp widths are powers of two");
    if (p->warp_lanes_max < p->warp_lanes_min)
        cfg.fail("warp.lanes_max", "warp.lanes_max is below warp.lanes_min");
    p->warp_lanes = p->warp_lanes_min; // (at most kMaxLanes)
    p->warp_max = key_u32(cfg, "warp.max");
    p->warp_capacity = key_u32(cfg, "warp.capacity");
    p->warp_step_cost = key_u32(cfg, "warp.step_cost");
    p->warp_lane_cost = key_u32(cfg, "warp.lane_cost");
    p->warp_rounds = key_u32(cfg, "warp.rounds");
    p->spark_gpu_grant = key_u32(cfg, "spark.gpu_grant", key_default(p->warps ? 8 : 0));
    p->spark_gpu_born = key_f64(cfg, "spark.gpu_born");
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
        p->warp_lane_cost = 0;
        set_warp_lanes(p, p->warp_lanes); // (its step cost)
        p->gpu_elements = clamp(tick / static_cast<double>(p->cost.gpu_thr));
        p->npu_macs = clamp(tick / (static_cast<double>(p->cost.npu_mac) * 1000.0));
    }
    // Phase 9c (design.md 6.15): async results come back after each device's
    // own round trip, in whole ticks, with a timed table; otherwise after
    // accel.latency, as before.
    p->gpu_latency = p->npu_latency = p->accel_latency;
    if (p->cost.timed) {
        auto ticks = [p](u64 ps) {
            const double t = static_cast<double>(ps) / p->tick_units;
            const double up = static_cast<double>(static_cast<u64>(t)) < t ? static_cast<double>(static_cast<u64>(t)) + 1
                                                                          : static_cast<double>(static_cast<u64>(t));
            return up < 1 ? 1u : (up > 1e9 ? 1000000000u : static_cast<u32>(up));
        };
        p->gpu_latency = ticks(p->cost.gpu_start);
        p->npu_latency = ticks(p->cost.npu_start);
    }
    if (p->warps)
        sense_n = kSenseReadingsWarp; // + my lane, lanes in my warp
    if (p->cost.timed)
        sense_n = kSenseReadingsActions; // + an action on me: ticks until it ends, what it is
    // Threads and cores (Phase 9c, design.md 6.15): off = exactly as before.
    p->threads = key_flag(cfg, "cpu.threads");
    p->slices = key_u32(cfg, "cpu.slices");
    p->max_threads = key_u32(cfg, "cpu.max_threads");
    p->e_share = key_f64(cfg, "cpu.e_share", key_default(p->cost.timed ? 0.5 : 0.0));
    p->e_gain = static_cast<float>(key_f64(cfg, "heat.e_gain"));
    if (p->e_share > 0 && !p->cost.timed) // an E-core's speed is the table's E column
        cfg.fail("cpu.e_share", "needs a timed cost table (cost.table = home_v1)");
    // Phase 10 (design.md 6.16): vents, decay, bites and death.
    // Ray's background flip rate (design.md 6.3): about 1 per 10,000
    // instructions executed, which at full load is 1e-4 * millicores *
    // instr_per_core / 1000 flips per tick, spread over the soup. Decay needs
    // a table whose codon 0 is INERT (nova_v2): under the others zeros are
    // labels, so there it stays off and rot keeps Ray's rate.
    const double nominal = static_cast<double>(p->millicores) * p->instr_per_core / 1000.0;
    const bool inert0 = p->table.op[0] == Op::inert;
    const double rot0 = 1e-4 * nominal / soup_size;
    const double auto_decay = inert0 ? (rot0 < 1 ? rot0 : 1.0) : 0.0;
    p->mem_decay = key_f64(cfg, "mem.decay", key_default(auto_decay));
    if (p->mem_decay < 0) // (below 0 means the default too)
        p->mem_decay = auto_decay;
    p->disk_decay = key_f64(cfg, "disk.decay", key_default(p->disk && p->table.op[0] == Op::inert ? 1e-6 : 0.0));
    p->archive_decay = key_f64(cfg, "archive.decay", key_default(p->disk_decay / 100)); // disk tiers (W4)
    p->vent_spacing = key_u32(cfg, "vent.spacing");
    p->vent_rate = key_u32(cfg, "vent.rate");
    p->vent_radius = key_u32(cfg, "vent.radius");
    p->bites = key_flag(cfg, "engulf.bite");
    p->bite_share = key_f64(cfg, "engulf.bite_share"); // docs/bites-plan.md B2, B4 (with bites)
    p->tissue = key_flag(cfg, "engulf.tissue");
    p->bleed_every = key_u32(cfg, "life.bleed_every"); // B6
    p->threads_end = key_flag(cfg, "life.threads_end");
    p->frozen_ticks = key_u32(cfg, "life.frozen_ticks");
    p->gestation = key_u32(cfg, "life.gestation"); // docs/lifehistory-plan.md G
    p->gestation_per_codon = key_f64(cfg, "life.gestation_per_codon");
    // Dormancy and drift (docs/dormancy-plan.md): the watchdog's frozen body
    // goes dormant instead of dying, if it has reserves to spend.
    // Voluntary dormancy (docs/lifehistory-plan.md V4): life.dormancy is the
    // automatic rule, life.sleep the instruction; either needs the machinery.
    p->dormancy_auto = key_flag(cfg, "life.dormancy");
    p->sleep = key_flag(cfg, "life.sleep");
    p->sleep_max = key_u32(cfg, "dormant.sleep_max");
    p->dormancy = p->dormancy_auto || p->sleep;
    p->dormant_mem_loss = key_u32(cfg, "dormant.mem_loss");
    p->dormant_disk_loss = key_u32(cfg, "dormant.disk_loss");
    p->dormant_dwell = key_u32(cfg, "dormant.dwell");
    p->disasters = key_flag(cfg, "disaster.enabled"); // docs/disasters-plan.md (the world reads the rest)
    p->kinds = p->threads || p->e_share > 0;
    if (p->kinds)
        sense_n = kSenseReadingsThreads; // + my threads, my kind, my E millicores, free E millicores
    m.sense_n = static_cast<u8>(sense_n);
    m.res_n = static_cast<u8>(p->e_share > 0 ? 6 : (p->npus ? 5 : 4)); // Phase 9c: 5 is E millicores
    if (p->archive) // disk tiers (W4): 6 is the archive (without one, the maps and draws are as before)
        m.res_n = 7;
    if (p->mem_fast_share > 0) // memory tiers (W3): 7 is fast memory (without tiers, as before)
        m.res_n = 8;
    identity_map(m.sense, kSenseReadingsMax);
    identity_map(m.disk, 3);
    identity_map(m.res, 8);
    identity_map(m.head, 2);
    identity_map(m.divide, 4);
    identity_map(m.gpu, kGpuOps);
    identity_map(m.npu, kNpuOps);
    if (sel.eq("shuffled")) {
        const u64 world_seed = key_u64(cfg, "seed");
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
        if (p->disk || p->archive) // compare (changes nothing) never stays at 0 (W4: an archive alone too)
            draw_selector_map(r, m.disk, 3);
        m.shuffled = 1;
    }
    p->open_membrane = key_choice(cfg, "isa.membrane").eq("open");
    p->stack_depth = key_u32(cfg, "isa.stack_depth"); // (at most kMaxStack)
    p->search_limit = key_u32(cfg, "isa.search_limit", key_max(soup_size));
    p->max_template = key_u32(cfg, "isa.max_template");
    p->tierra_share = key_u32(cfg, "tierra.divide_share");

    p->spark_rate = key_f64(cfg, "spark.rate");
    p->spark_initial = key_u32(cfg, "spark.initial");
    p->spark_grant = key_u32(cfg, "spark.grant");
    p->spark_min = key_u32(cfg, "spark.min_len", key_max(soup_size));
    p->spark_max = key_u32(cfg, "spark.max_len", key_default_max(128 < soup_size ? 128 : soup_size, soup_size));
    if (p->spark_min > p->spark_max)
        cfg.fail("spark.min_len", "must not exceed spark.max_len");
    // A varied world (docs/world-plan.md): sparks by activity (W5), and the
    // substrate's multipliers (W1), which the world sets per node (1 here).
    p->spark_activity = key_f64(cfg, "spark.activity");
    p->p_speed = p->e_speed = p->gpu_speed = p->npu_speed = 1.0f;
    p->mem_fast_speed = p->mem_slow_speed = p->disk_speed = p->archive_speed = 1.0;

    p->heat_on = key_flag(cfg, "heat.enabled");
    p->cool = static_cast<float>(key_f64(cfg, "heat.cool"));
    p->cpu_gain = static_cast<float>(key_f64(cfg, "heat.cpu_gain"));
    p->fetch_gain = static_cast<float>(key_f64(cfg, "heat.fetch_gain"));
    p->write_gain = static_cast<float>(key_f64(cfg, "heat.write_gain"));
    p->cpu = get_curve(cfg, "heat.cpu");
    p->mem = get_curve(cfg, "heat.mem");
    // The GPU runs hot: flat out it settles at 90, deep in its throttle band.
    p->gpu_gain = static_cast<float>(key_f64(cfg, "heat.gpu_gain"));
    p->gpu = get_curve(cfg, "heat.gpu");
    p->npu_gain = static_cast<float>(key_f64(cfg, "heat.npu_gain"));
    p->npu = get_curve(cfg, "heat.npu");
    // The ambient leak (Phase 4): 0 = off, every unit's heat its own.
    p->ambient = static_cast<float>(key_f64(cfg, "heat.ambient"));

    // Ray's baselines (design.md 6.3): about 1 copy error per 2,000 codons
    // copied; about 1 background flip per 10,000 instructions executed (rot0,
    // above).
    p->copy_rate = key_f64(cfg, "hazard.copy");
    p->flaw_rate = key_f64(cfg, "hazard.flaw");
    // Phase 10 (design.md 6.16): rot is now cosmic rays, a hundredth of Ray's
    // background rate by default (hazard.rot_scale); dead matter decays at
    // that rate instead (mem.decay).
    const double rot_scale = key_f64(cfg, "hazard.rot_scale", key_default(p->mem_decay > 0 ? 0.01 : 1.0));
    p->rot_rate = key_f64(cfg, "hazard.rot", key_default(rot0 * rot_scale < 1 ? rot0 * rot_scale : 1.0));
    // Rays (docs/lifehistory-plan.md R): off keeps the independent flips.
    p->rays = key_flag(cfg, "hazard.rays");
    p->ray_cosmic = key_f64(cfg, "hazard.ray_cosmic");
    p->ray_static = key_f64(cfg, "disaster.static_energy");
    p->ray_strike = key_f64(cfg, "disaster.strike_energy");
    p->ray_flip = key_f64(cfg, "hazard.ray_flip");
    p->ray_flip_nop = key_f64(cfg, "hazard.ray_flip_nop");
    p->ray_absorb = key_f64(cfg, "hazard.ray_absorb");
    p->ray_absorb_waste = key_f64(cfg, "hazard.ray_absorb_waste");
    p->ray_absorb_nop = key_f64(cfg, "hazard.ray_absorb_nop");
    p->ray_indel = key_f64(cfg, "hazard.ray_indel"); // R7 (only with rays: hazard.rays)
    p->ray_indel_energy = key_f64(cfg, "hazard.ray_indel_energy");

    p->contact_range = key_u32(cfg, "contact.range", key_max(soup_size));
    p->contact_base = key_u32(cfg, "contact.cost_base");
    p->contact_per_codon = key_f64(cfg, "contact.cost_per_codon");
    // INJECT needs a host it fits in (Phase 1b, design.md 6.6), and lands only
    // through an open end (docs/bites-plan.md I1; the key it needed is gone).
    p->inject_room = key_f64(cfg, "contact.inject_room");
    p->inject_ratio = key_f64(cfg, "contact.inject_ratio"); // docs/inject-plan.md M6
    p->digest = key_u32(cfg, "engulf.digest");               // S2

    Str genome = key_str(cfg, "seed.genome");
    p->seed_at = key_u32(cfg, "seed.at", key_max(soup_size - 1));
    p->seed_millicores = key_u32(cfg, "seed.millicores");
    p->seed_gpus = key_u32(cfg, "seed.gpus");
    p->seed_npus = key_u32(cfg, "seed.npus");
    p->seed_form = key_choice(cfg, "seed.form").eq("gpu") ? kFormGpu : kFormCpu;
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

// Memory tiers (docs/world-plan.md W3): whole pages, so a page is all of one
// tier (volatility and heat are per page).
u32 mem_fast_end(u32 size, double share) {
    if (!(share > 0) || size < kPage)
        return 0;
    if (share >= 1)
        return size;
    u64 pages = static_cast<u64>(static_cast<double>(size) * share) >> kPageBits;
    if (pages == 0)
        pages = 1;
    return static_cast<u32>(pages << kPageBits);
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
    prof_ = nullptr; // behaviour profiles (A3): likewise

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
    actions_.clear();   // Phase 9
    cpu_kind_.clear();  // Phase 9c

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
    // Phase 9c (design.md 6.15): the last round(m x cpu.e_share) units are E
    // millicores, in a pool of their own.
    e_first_ = 0xFFFFFFFFu;
    epool_.clear();
    eunits_.clear();
    espeed_.clear();
    eheat_.clear();
    ework_.clear();
    cpu_time_.clear();
    cpu_etime_.clear();
    const u32 me = p->e_share > 0 ? static_cast<u32>(floor(static_cast<double>(m) * p->e_share + 0.5)) : 0;
    if (me > 0) {
        e_first_ = m - me;
        pool_.resize(e_first_);
        epool_.resize(me);
        for (u32 i = 0; i < me; ++i) {
            epool_[i] = e_first_ + i;
            pool_pos_[e_first_ + i] = i;
        }
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
    // memory and sparks above are drawn exactly as without one. W4: the
    // archive follows the fast disk in the same ledger (and the same files),
    // its contents the same stream's next draws.
    if (p->disk || p->archive) {
        // Phase 7b: on real disk, in this node's files (bound once, kept for
        // every life of the node).
        if (disk_folder_ && !disk_.mem.in_file() && !disk_.bind_files(disk_folder_, node))
            NOVA_FATAL("Soup: cannot create the disk tier's files in disk.path");
        Rng dr(seed ^ 0x6469736B00000000ull, node, Rng::kInitTick - life); // "disk"
        disk_.init(p->disk + p->archive, dr, p->disk);
    } else {
        disk_.init(0, rng_, 0);
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
        disk_.stamp.resize(disk_.size()); // (W4: both tiers)
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
    wrec_gpu_.clear();
    wbody_chain_.clear();
    wround_ = 0;
    wcst_.clear();
    if (p->warps)
        build_warp_consts();
    // The lifecycle (Phase 4): a fresh life.
    writes_ = 0;
    collapse_ = false;
    hazard_mult_ = 1.0;
    // The allocator's "no room of this length" cache: a new life's memory is
    // all waste, so nothing an earlier life noted holds (kept, a reborn node
    // refused its first allocations that long until a codon was freed).
    free_epoch_ = 0;
    fail_epoch_ = ~0ull;
    fail_len_ = 0;
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
        // W4: each tier's overlap with its own old tier (rf: where the old
        // archive began; without archives both are the whole disk).
        const u32 rn = static_cast<u32>(ruins->disk.size());
        const u32 rf = min_of(ruins->disk_fast, rn);
        const u32 dov = min_of(disk_.fast, rf);
        const u32 aov = min_of(disk_.size() - disk_.fast, rn - rf);
        for (u32 i = 0; i < dov; ++i)
            disk_.mem[i] = ruins->disk[i];
        for (u32 i = 0; i < aov; ++i)
            disk_.mem[disk_.fast + i] = ruins->disk[rf + i];
        if (!disk_.stamp.empty()) {
            for (u32 i = 0; i < dov && i < ruins->disk_stamp.size(); ++i)
                disk_.stamp[i] = ruins->disk_stamp[i];
            for (u32 i = 0; i < aov && rf + i < ruins->disk_stamp.size(); ++i)
                disk_.stamp[disk_.fast + i] = ruins->disk_stamp[rf + i];
        }
        weather(disk_.mem.data(), dov, p->disk_rot * static_cast<double>(ruins->fallow));
        weather(disk_.mem.data() + disk_.fast, aov, p->archive_rot * static_cast<double>(ruins->fallow));
        // Phase 10: and decayed (dead matter loses bits; past 8 expected
        // losses a codon, simply gone).
        auto fade = [&wr](u8* m, u32 len, double lambda) {
            if (lambda <= 0 || len == 0)
                return;
            if (lambda >= 8) {
                for (u32 i = 0; i < len; ++i)
                    m[i] = 0;
                return;
            }
            double pos = -log(1.0 - wr.unit()) / lambda;
            while (pos < len) {
                u8& c = m[static_cast<u32>(pos)];
                if (c)
                    c = fade_bit(c, wr);
                pos += -log(1.0 - wr.unit()) / lambda;
            }
        };
        fade(mem_.data(), mo, p->mem_decay * static_cast<double>(ruins->fallow));
        fade(disk_.mem.data(), dov, p->disk_decay * static_cast<double>(ruins->fallow));
        fade(disk_.mem.data() + disk_.fast, aov, p->archive_decay * static_cast<double>(ruins->fallow)); // W4
    }
    // Phase 10: the vents' places for this life, from their own stream (so
    // every other draw is as without them).
    vent_at_.clear();
    last_ran_.clear();
    dorm_.clear();
    dorm_tag_.clear();
    sleep_req_.clear();
    origin_.clear();
    guest_host_.clear(); // docs/inject-plan.md: no guests, no stomachs
    guest_kind_.clear();
    guest_at_.clear();
    guest_len_.clear();
    stomach_.clear();
    guests_.clear();
    offline_.clear(); // disasters: every core online, nothing brewing
    pending_wipes_.clear(); // docs/lifehistory-plan.md W: no wipe coming
    weather_ = Weather{};
    clear_fever();
    if (p->vent_spacing > 0) {
        const u32 nv = n / p->vent_spacing > 0 ? n / p->vent_spacing : 1;
        Rng vr(seed ^ 0x76656E7473000000ull, node, Rng::kInitTick - life); // "vents"
        for (u32 i = 0; i < nv; ++i)
            vent_at_.push(static_cast<u32>(vr.below(n)));
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
    bl.born = p_->gestation > 0 ? static_cast<u32>(tick_) : 0; // (G3: only then, so hashes hold without it)
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
    // docs/inject-plan.md M2: its parasites' threads may run in it too.
    if (has_guests(body))
        for (usize i = 0; i < order_.size(); ++i) {
            const u32 g = order_[i];
            if (guest_host_[g] == body && guest_kind_[g] == kGuestParasite)
                for (u32 c = bodies_[g].cpus; c != kNone; c = cpus_[c].next)
                    if (cpus_[c].block == k)
                        cpus_[c].block = kNone;
        }
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
    if (b < sleep_req_.size()) // (V1: nothing asked yet)
        sleep_req_[b] = 0;
    if (p_->gpus)
        gpu_.body_slot(b, bodies_.size());
    if (p_->npus)
        npu_.body_slot(b, bodies_.size());
    if (disk_on()) // (W4: a fast disk or an archive)
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
        if (b < wbody_chain_.size()) // (the split's guess: a new body's warps haven't run)
            wbody_chain_[b] = 0;
    }
    if (ek()) { // Phase 9c: no E millicores yet
        if (eunits_.size() < bodies_.size()) {
            eunits_.resize(bodies_.size());
            espeed_.resize(bodies_.size());
            eheat_.resize(bodies_.size());
            ework_.resize(bodies_.size());
        }
        eunits_[b] = 0;
        espeed_[b] = eheat_[b] = 0.0f;
        ework_[b] = 0;
    }
    if (p_->frozen_ticks > 0) { // Phase 10: born now counts as running now
        if (last_ran_.size() < bodies_.size())
            last_ran_.resize(bodies_.size());
        last_ran_[b] = tick_;
    }
    if (p_->dormancy) { // not dormant
        if (dorm_.size() < bodies_.size()) {
            dorm_.resize(bodies_.size());
            dorm_tag_.resize(bodies_.size());
        }
        dorm_[b] = Dormant{};
        dorm_tag_[b] = CargoTag{};
    }
    if (origin_.size() < bodies_.size()) // W7: how it began, set by whoever made it
        origin_.resize(bodies_.size());
    origin_[b] = kOriginUnknown;
    if (guest_host_.size() < bodies_.size()) { // docs/inject-plan.md: free, no stomach
        guest_host_.resize(bodies_.size());
        guest_kind_.resize(bodies_.size());
        guest_at_.resize(bodies_.size());
        guest_len_.resize(bodies_.size());
        stomach_.resize(bodies_.size());
        guests_.resize(bodies_.size());
    }
    guest_host_[b] = kNone; // (a slot's last life left no guest state: kill, absorb)
    guest_kind_[b] = kGuestNone;
    guest_at_[b] = guest_len_[b] = 0;
    guests_[b] = 0;
    stomach_[b] = kNone;
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
    if (kinds_on()) { // Phase 9c: a new thread prefers P, unless its maker says otherwise
        while (cpu_kind_.size() < cpus_.size())
            cpu_kind_.push(kKindP);
        cpu_kind_[c] = kKindP;
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
    claim_mixed(b, units);
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
    claim_mixed(b, units);
    if (gpus)
        claim_gpus(b, gpus);
    birth(b, start, form);
    return b;
}

bool Soup::arrive(const u8* code, u32 len, u32 millicores, u32 gpus, u32 npus, u32 disk, const CargoTag& tag,
                  u32 form, u32 emillicores, u8 kind, u32 archive) {
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
        if (!ek()) {
            b = create_body(at, len, millicores);
            NOVA_ASSERT(b != kNone);
        } else { // Phase 9c: each kind from its own pool (capped)
            b = new_body();
            new_block(at, len, b);
            new_cpu(b, at);
            claim_units(b, millicores);
            claim_e_units(b, emillicores);
        }
        if (gpus && p_->gpus)
            claim_gpus(b, gpus);
    } else { // Phase 8: born in her form, once she holds her shares
        b = new_body();
        new_block(at, len, b);
        claim_units(b, millicores);
        if (ek())
            claim_e_units(b, emillicores);
        if (gpus && p_->gpus)
            claim_gpus(b, gpus);
    }
    if (npus && p_->npus)
        claim_npus(b, npus);
    if (disk && p_->disk) // her disk share, empty, capped by what's free (Phase 4)
        disk_.claim_any(rng_, b, disk);
    if (archive && p_->archive) // W4: and her archive share, the same way in the archive
        disk_.claim_any(rng_, b, archive, true);
    if (p_->warps)
        birth(b, at, form);
    if (kinds_on() && bodies_[b].cpus != kNone) // Phase 9c: her thread prefers her divider's kind
        cpu_kind_[bodies_[b].cpus] = kind;
    origin_[b] = kOriginArrival;
    if (obs_)
        obs_->on_arrive(*this, owner_[at], tag, tick_);
    ++tally_.arrived;
    ++arrived_all_;
    if (bodies_[b].units == 0)
        ++arrived_dry_; // (diagnostics: no free millicore here, so she dies in the next sweep)
    return true;
}

void Soup::kill(u32 b) {
    // docs/inject-plan.md S2: a swallowed body that dies before it is
    // digested is food at once.
    if (guest_kind_[b] == kGuestSwallowed && guest_host_[b] != kNone && bodies_[guest_host_[b]].alive) {
        absorb(b, guest_host_[b]);
        ++tally_.deaths;
        return;
    }
    if (obs_) // (a membership it carries on its body, owning no block: a parasite's)
        obs_->on_guest_gone(*this, b, tick_);
    set_guest(b, kNone, kGuestNone);
    GuestRelease rel; // M5, S5: its guests outlive it
    list_guests(b, &rel);
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
    if (disk_on()) // the disk is freed; its contents stay as fossils (W4: both tiers)
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
    if (b < dorm_.size())
        dorm_[b].on = 0;
    if (b < sleep_req_.size()) // (V1: a sleep it asked for dies with it)
        sleep_req_[b] = 0;
    ++tally_.deaths;
    free_guests(rel);
}

// --- dormancy and drift (docs/dormancy-plan.md) ------------------------------

// A random open neighbour (one whose path isn't down) from this node's stream,
// as an index into its ports, or kNone with none (no dispersal, a hermit, or
// every path down).
u32 Soup::drift_port() {
    const DispersalPorts& P = ports_;
    u32 open = 0;
    for (u32 k = 0; k < P.degree; ++k)
        open += !(P.down && P.down[P.edges[k]]);
    if (open == 0)
        return kNone;
    u32 pick = static_cast<u32>(rng_.below(open));
    for (u32 k = 0; k < P.degree; ++k)
        if (!(P.down && P.down[P.edges[k]]) && pick-- == 0)
            return k;
    return kNone;
}

// A frozen body goes dormant (the death sweep, in place of the watchdog's
// kill): it keeps its code (its first block) and reserves (the memory in its
// other blocks, its disk holding) and remembers the cores it held; everything
// else goes back to this node -- its millicores and GPU/NPU units to the pools
// with their heat, its other blocks to waste with their contents, its disk as
// fossils, its warps and threads. Then it drifts to a random neighbour, or
// rests here with nowhere to go. With no reserves it has no time to search:
// false, and it dies as before.
//
// Voluntary dormancy (docs/lifehistory-plan.md V1-V2, V6): with `sleep` > 0
// it chose this, for that many ticks (its timer in Dormant::spare); with
// `drift` it drifts as above, else it rests here throughout (kDormantStays).
// Without reserves it dies all the same: to sleep usefully a program sets
// memory aside first.
bool Soup::go_dormant(u32 b, u64 offline_until, u32 grace, u32 sleep, bool drift) {
    Body& B = bodies_[b];
    const u32 k = B.blocks;
    if (k == kNone)
        return false;
    // docs/inject-plan.md M8, S3: no guest goes dormant, nor a host with
    // guests in it (the caller kills it instead, freeing them).
    if (guest_kind_[b] != kGuestNone || has_guests(b))
        return false;
    const u32 code = blocks_[k].owned;
    const u32 mem_reserve = B.mem > code ? B.mem - code : 0;
    const u32 disk_reserve = disk_on() ? disk_.total[b] : 0; // (W4: both tiers)
    const bool bare = mem_reserve == 0 && disk_reserve == 0;
    if (code == 0 || (bare && grace == 0)) {
        if (offline_until == 0 && sleep == 0) // (a burnout's victims aren't frozen, nor are sleepers)
            ++frozen_bare_;
        return false;
    }
    if (prof_) // behaviour profiles (A3): a followed body's sample ends here, holdings read first
        prof_->on_dormant(*this, b);
    if (sleep && obs_) // V7: the sleeper milestone, while its lineage still holds it
        obs_->on_sleep(*this, k, tick_);
    Dormant d = {};
    d.on = 1;
    d.dwell = p_->dormant_dwell;
    d.mem_reserve = bare ? grace : mem_reserve; // disasters X4: a bare burnout victim's grace reserve
    // W4: each tier's reserve apart (the clock spends the fast disk's first;
    // waking claims each tier's).
    d.archive_reserve = disk_.tiered() ? disk_.held_in(b, true) : 0;
    d.disk_reserve = disk_reserve - d.archive_reserve;
    d.flags = static_cast<u8>((offline_until ? kDormantBurnt : 0) | (bare ? kDormantGraced : 0)); // (disasters X4)
    if (sleep) { // V6
        d.flags = static_cast<u8>(d.flags | kDormantVoluntary | (drift ? 0 : kDormantStays));
        d.spare = static_cast<u16>(sleep < 65535u ? sleep : 65535u);
    }
    const u32 e = e_units(b);
    d.millicores = B.units - e;
    d.emillicores = e;
    d.gpus = p_->gpus ? gpu_.count[b] : 0;
    d.npus = p_->npus ? npu_.count[b] : 0;
    d.form = B.form;
    d.kind = kinds_on() && B.cpus != kNone ? cpu_kind_[B.cpus] : kKindP;
    // Its lineage tag waits with it, and it leaves its lineage now, wherever
    // it will rest; it rejoins on waking (X12). So neither its departure nor
    // its death while dormant leaves the lineage again.
    dorm_tag_[b] = obs_ ? obs_->cargo_tag(k) : CargoTag{};
    if (obs_)
        obs_->on_dormant(k, tick_);
    const u32 g = genome_len(b);
    for (u32 j = blocks_[k].next; j != kNone;) {
        const u32 next = blocks_[j].next;
        stamp_block(j, g);
        free_block(j);
        j = next;
    }
    blocks_[k].next = kNone;
    B.mem = blocks_[k].owned;
    B.latest = kNone;
    while (B.unit_head != kNone) {
        const u32 u = B.unit_head;
        unlink_unit(u);
        if (offline_until) // disasters X4: burnt, offline for repair (no one's, not free)
            offline_.push(OfflineCore{offline_until, u, 0});
        else
            pool_push(u);
    }
    if (p_->gpus && offline_until) {
        while (gpu_.head[b] != kNone) {
            const u32 u = gpu_.head[b];
            gpu_.unlink(u);
            offline_.push(OfflineCore{offline_until, u | kOfflineGpu, 0});
        }
    } else if (p_->gpus) {
        gpu_.release(b, gpu_.count[b]);
    }
    if (p_->npus)
        npu_.release(b, npu_.count[b]);
    if (disk_on()) // (W4: both tiers)
        disk_.release_tail(b, disk_.total[b], g);
    if (p_->warps)
        kill_warps(b);
    for (u32 c = B.cpus; c != kNone;) {
        const u32 next = cpus_[c].next;
        cpus_[c].body = kNone;
        free_cpus_.push(c);
        c = next;
    }
    B.cpus = kNone;
    B.ncpu = 0;
    B.carry = 0.0f; // its upkeep debt goes with its cores: it will start afresh, as an arrival does
    B.work = 0;
    B.speed = 0.0f;
    B.err = 1.0f;
    dorm_[b] = d;
    ++tally_.dormant_started;
    dormant_mem_sum_ += mem_reserve;
    dormant_disk_sum_ += disk_reserve;
    if (offline_until) // disasters X4: a burnout's victim rests here for its dwell first
        return true;
    if (sleep && !drift) // V2: BX 1 rests where it is
        return true;
    const u32 nbr = drift_port();
    if (nbr != kNone)
        depart_dormant(b, nbr);
    return true;
}

// A dormant program leaves for neighbour nbr: its code becomes cargo (outside
// the edge's capacity) and the block it rested in waste, keeping its contents,
// as a departing daughter's does. Its lineage tag goes with it.
void Soup::depart_dormant(u32 b, u32 nbr) {
    Dormant& d = dorm_[b];
    Body& B = bodies_[b];
    const u32 k = B.blocks;
    const Block& K = blocks_[k];
    Departure dep = {};
    dep.tag = dorm_tag_[b];
    dep.nbr = nbr;
    dep.len = K.len;
    dep.codon_off = static_cast<u32>(departure_codons_.size());
    departure_codons_.append(mem_.data() + K.start, K.len);
    dep.millicores = d.millicores;
    dep.emillicores = d.emillicores;
    dep.gpus = d.gpus;
    dep.npus = d.npus;
    dep.disk = d.disk_reserve;
    dep.archive = d.archive_reserve; // W4
    dep.form = d.form;
    dep.kind = d.kind;
    dep.dormant = 1;
    dep.mem_reserve = d.mem_reserve;
    dep.hops = d.hops + 1;
    if (d.flags & kDormantVoluntary) // V: a drifting sleeper's timer travels with it
        dep.sleep = (static_cast<u32>(d.flags & (kDormantVoluntary | kDormantStays)) << 16) | d.spare;
    stamp_block(k, K.len);
    free_block(k);
    B.blocks = B.latest = kNone;
    B.mem = 0;
    B.alive = 0;
    d.on = 0;
    departures_.push(dep);
    ++tally_.dormant_hops;
}

// Whether a core of its kind is free here: a GPU unit for a GPU-born program
// that held one, else a millicore of a kind it held.
bool Soup::can_wake(u32 b) const {
    const Dormant& d = dorm_[b];
    if (d.form == kFormGpu && d.gpus > 0)
        return p_->gpus > 0 && !gpu_.pool.empty();
    if (!ek())
        return !pool_.empty();
    return (d.millicores > 0 && !pool_.empty()) || (d.emillicores > 0 && !epool_.empty());
}

// It wakes where it rests: it claims up to the cores it held at home, capped
// by what's free here, and up to its disk reserve (empty), then starts as an
// arrival does -- one thread at its start, registers zero, in its own form.
// Its memory reserve was its fuel and is spent. Here it rejoins its lineage
// (it left when it went dormant), as an arrival joins hers.
void Soup::wake(u32 b) {
    Dormant& d = dorm_[b];
    Body& B = bodies_[b];
    const u32 start = blocks_[B.blocks].start;
    if (ek()) {
        claim_units(b, d.millicores);
        claim_e_units(b, d.emillicores);
    } else {
        claim_units(b, d.millicores + d.emillicores);
    }
    if (d.gpus && p_->gpus)
        claim_gpus(b, d.gpus);
    if (d.npus && p_->npus)
        claim_npus(b, d.npus);
    if (d.disk_reserve && p_->disk)
        disk_.claim_any(rng_, b, d.disk_reserve);
    if (d.archive_reserve && p_->archive) // W4: each tier it held, capped by that tier's free codons
        disk_.claim_any(rng_, b, d.archive_reserve, true);
    d.on = 0;
    if (p_->warps)
        birth(b, start, d.form);
    else
        new_cpu(b, start);
    if (kinds_on() && B.cpus != kNone)
        cpu_kind_[B.cpus] = static_cast<u8>(d.kind);
    if (p_->frozen_ticks > 0)
        last_ran_[b] = tick_;
    origin_[b] = kOriginWake;
    if (obs_)
        obs_->on_arrive(*this, B.blocks, dorm_tag_[b], tick_);
    ++tally_.dormant_woke;
    tally_.sleep_woke += (d.flags & kDormantVoluntary) ? 1 : 0; // V7
}

// A resting dormant program's end of tick (the death sweep, in schedule
// order): it wakes if a core of its kind is free; else the clock spends its
// memory reserve, then its disk reserve, and with neither left (or its code
// gone to bites or heat) it dies; else, its stay over, it moves on to a random
// neighbour (staying with nowhere to go).
//
// Voluntary dormancy (docs/lifehistory-plan.md V2-V3): a sleeper's timer runs
// down first, and it never wakes before it's up; then it wakes as any dormant
// program does, or waits on its reserves. Resting in place (BX 1), it never
// moves on.
void Soup::rest_tick(u32 b) {
    Dormant& d = dorm_[b];
    const bool vol = (d.flags & kDormantVoluntary) != 0;
    if (bodies_[b].mem == 0) {
        ++tally_.dormant_expired;
        tally_.sleep_expired += vol ? 1 : 0;
        kill(b);
        return;
    }
    if (d.spare > 0) // (0 for every program that didn't choose to sleep)
        --d.spare;
    if (d.spare == 0 && can_wake(b)) {
        wake(b);
        return;
    }
    if (d.mem_reserve > 0)
        d.mem_reserve -= d.mem_reserve < p_->dormant_mem_loss ? d.mem_reserve : p_->dormant_mem_loss;
    else if (d.disk_reserve > 0)
        d.disk_reserve -= d.disk_reserve < p_->dormant_disk_loss ? d.disk_reserve : p_->dormant_disk_loss;
    else if (d.archive_reserve > 0) // W4: the archive's reserve last, at the disk's rate
        d.archive_reserve -= d.archive_reserve < p_->dormant_disk_loss ? d.archive_reserve : p_->dormant_disk_loss;
    if (d.mem_reserve == 0 && d.disk_reserve == 0 && d.archive_reserve == 0) {
        ++tally_.dormant_expired;
        tally_.sleep_expired += vol ? 1 : 0;
        kill(b);
        return;
    }
    if (d.flags & kDormantStays) // V2: resting where it chose to
        return;
    if (d.dwell > 1) {
        --d.dwell;
        return;
    }
    const u32 nbr = drift_port();
    if (nbr == kNone) {
        d.dwell = p_->dormant_dwell;
        return;
    }
    depart_dormant(b, nbr);
}

bool Soup::land_dormant(const u8* code, u32 len, const Dormant& d, const CargoTag& tag) {
    if (len == 0 || len > p_->size || !p_->dormancy)
        return false;
    const u32 from = static_cast<u32>(rng_.below(p_->size));
    u32 at = known_full(len) ? kNone : find_free_fwd(from, len);
    if (at == kNone && !known_full(len))
        at = find_free_fwd(0, len);
    if (at == kNone) {
        note_full(len);
        return false;
    }
    for (u32 i = 0; i < len; ++i)
        mem_[at + i] = code[i];
    const u32 b = new_body();
    new_block(at, len, b);
    bodies_[b].form = d.form;
    dorm_[b] = d;
    dorm_[b].on = 1;
    dorm_[b].dwell = p_->dormant_dwell;
    dorm_tag_[b] = tag;
    return true;
}

u32 Soup::resting_count() const {
    if (!p_->dormancy)
        return 0;
    u32 n = 0;
    for (usize k = 0; k < order_.size(); ++k)
        n += dorm_[order_[k]].on != 0;
    return n;
}

// --- bites as wounds, bleeding (docs/bites-plan.md B5, B6) --------------------

u32 Soup::outer_codon(u32 b, bool high) const {
    const u32 k = bodies_[b].blocks;
    if (k == kNone || guest_kind_[b] != kGuestNone) // (a guest has no surface of its own: inject-plan M8, S3)
        return kNone;
    const Block& bl = blocks_[k];
    if (bl.owned < 2)
        return kNone; // down to one codon: nothing left to bleed or to enter by
    if (high) {
        for (u32 a = bl.start + bl.len; a > bl.start; --a)
            if (owner_[a - 1] == k)
                return a - 1;
    } else {
        for (u32 a = bl.start; a < bl.start + bl.len; ++a)
            if (owner_[a] == k)
                return a;
    }
    return kNone;
}

bool Soup::open_end(u32 b, bool high) const {
    const u32 a = outer_codon(b, high);
    return a != kNone && !is_nop(p_->table.op[mem_[a]]);
}

u64 Soup::open_ends() const {
    u64 n = 0;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 b = order_[i];
        if (bodies_[b].alive && bodies_[b].blocks != kNone)
            n += static_cast<u64>(open_end(b, false)) + static_cast<u64>(open_end(b, true));
    }
    return n;
}

// Hosts with at least one living parasite in them now (docs/inject-plan.md).
u64 Soup::infected() const {
    Array<u8> seen;
    u64 n = 0;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 g = order_[i];
        if (!bodies_[g].alive || guest_kind_[g] != kGuestParasite || guest_host_[g] == kNone)
            continue;
        if (seen.empty())
            seen.resize(bodies_.size()); // (zeroed)
        const u32 h = guest_host_[g];
        n += seen[h] ? 0u : 1u;
        seen[h] = 1;
    }
    return n;
}

u64 Soup::guests_now(u8 kind) const {
    u64 n = 0;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 b = order_[i];
        n += bodies_[b].alive && guest_kind_[b] == kind ? 1u : 0u;
    }
    return n;
}

// --- guests (docs/inject-plan.md) ----------------------------------------------

void Soup::set_guest(u32 b, u32 host, u8 kind, u32 at, u32 len) {
    if (guest_kind_[b] != kGuestNone && guest_host_[b] != kNone)
        --guests_[guest_host_[b]];
    if (kind != kGuestNone && host != kNone)
        ++guests_[host];
    guest_host_[b] = host;
    guest_kind_[b] = kind;
    guest_at_[b] = at;
    guest_len_[b] = len;
}

// M5, S5: guest g's host died, its first block having spanned [host_lo,
// host_hi) (kNone first block: nothing). A parasite's region, clipped to
// that span, becomes its own block where it is still free (the longest
// free run of it); its threads in it run there, the rest go to its start.
// With none of it left it owns nothing and its threads fault.
void Soup::free_guest(u32 g, u32 host_first, u32 host_lo, u32 host_hi) {
    const u8 kind = guest_kind_[g];
    const u32 at = guest_at_[g], len = guest_len_[g];
    set_guest(g, kNone, kGuestNone);
    ++tally_.freed;
    if (kind != kGuestParasite)
        return;
    Body& B = bodies_[g];
    for (u32 c = B.cpus; c != kNone; c = cpus_[c].next) { // (its host's blocks are gone)
        cpus_[c].parasite = 0;
        cpus_[c].block = kNone;
    }
    if (host_first == kNone)
        return;
    const u32 lo = max_of<u32>(at, host_lo), hi = min_of<u32>(at + len, host_hi);
    u32 best = kNone, best_len = 0;
    for (u32 a = lo; a < hi;) {
        if (owner_[a] != kNone) {
            ++a;
            continue;
        }
        u32 e = a;
        while (e < hi && owner_[e] == kNone)
            ++e;
        if (e - a > best_len) {
            best = a;
            best_len = e - a;
        }
        a = e;
    }
    if (best == kNone)
        return;
    // Its own block now, first in its list (its code: the surface), before
    // any daughter block it was carving.
    const u32 had = B.blocks;
    B.blocks = kNone;
    const u32 k = new_block(best, best_len, g);
    blocks_[k].next = had;
    if (obs_)
        obs_->on_guest_freed(*this, g, k, tick_);
    for (u32 c = B.cpus; c != kNone; c = cpus_[c].next) {
        Cpu& C = cpus_[c];
        if (C.ip < best || C.ip >= best + best_len)
            C.ip = best;
        C.block = k;
    }
}

// S2: a digested body's end, or a swallowed body dying first: its eater gets
// what is left of it (as a whole swallow gave before digestion): memory,
// millicores with their heat, GPU and NPU units, disk, warps. Its threads end.
void Soup::absorb(u32 prey, u32 eater) {
    Body& P = bodies_[prey];
    Body& E = bodies_[eater];
    rehost_guests(prey, eater); // its own guests: inside the eater now
    if (!stamp_.empty()) { // its code stays its (salvage)
        const u32 g = genome_len(prey);
        for (u32 k = P.blocks; k != kNone; k = blocks_[k].next)
            stamp_block(k, g);
    }
    // Its blocks with anything left in them become the eater's; digestion's
    // emptied ones go.
    u32 tail = E.blocks;
    while (tail != kNone && blocks_[tail].next != kNone)
        tail = blocks_[tail].next;
    for (u32 k = P.blocks; k != kNone;) {
        const u32 nk = blocks_[k].next;
        blocks_[k].next = kNone;
        if (blocks_[k].owned == 0) {
            forget_block(prey, k);
            free_block(k);
        } else {
            blocks_[k].body = eater;
            if (tail == kNone)
                E.blocks = k;
            else
                blocks_[tail].next = k;
            tail = k;
        }
        k = nk;
    }
    E.mem += P.mem;
    for (u32 c = P.cpus; c != kNone;) {
        const u32 next = cpus_[c].next;
        cpus_[c].body = kNone;
        free_cpus_.push(c);
        c = next;
    }
    if (P.unit_head != kNone) {
        for (u32 u = P.unit_head; u != kNone; u = unit_next_[u])
            unit_owner_[u] = eater;
        if (E.unit_tail != kNone) {
            unit_next_[E.unit_tail] = P.unit_head;
            unit_prev_[P.unit_head] = E.unit_tail;
        } else {
            E.unit_head = P.unit_head;
        }
        E.unit_tail = P.unit_tail;
        E.units += P.units;
        if (ek()) {
            eunits_[eater] += eunits_[prey];
            eunits_[prey] = 0;
        }
    }
    if (p_->gpus)
        gpu_.move(prey, eater, gpu_.count[prey]);
    if (p_->npus)
        npu_.move(prey, eater, npu_.count[prey]);
    if (disk_on())
        disk_.move_all(prey, eater);
    if (p_->warps)
        merge_warps(prey, eater);
    P.blocks = P.cpus = P.latest = kNone;
    P.unit_head = P.unit_tail = kNone;
    P.units = P.mem = P.ncpu = 0;
    P.alive = 0; // the sweep recycles its slot
    set_guest(prey, kNone, kGuestNone);
    if (prey < sleep_req_.size())
        sleep_req_[prey] = 0;
    if (dormant(prey)) {
        dorm_[prey].on = 0;
        ++tally_.dormant_eaten;
    }
    if (obs_)
        obs_->on_guest_gone(*this, prey, tick_);
    ++tally_.engulfs; // (eaten, as a whole swallow counted before digestion)
}

bool Soup::has_guests(u32 h) const { return h < guests_.size() && guests_[h] > 0; }

// Body `from` is eaten whole by `to` (a swallowed body digested, or the
// whole-body ENGULF): its guests are inside `to` now, their places kept.
void Soup::rehost_guests(u32 from, u32 to) {
    if (!has_guests(from))
        return;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 g = order_[i];
        if (g != from && g != to && guest_host_[g] == from && guest_kind_[g] != kGuestNone)
            set_guest(g, to, guest_kind_[g], guest_at_[g], guest_len_[g]);
    }
}

// Body h is going (dying or eaten): its guests, listed now, while its first
// block's span is known (M5, S5). free_guests() frees them once its blocks
// are waste.
void Soup::list_guests(u32 h, GuestRelease* r) const {
    r->first = bodies_[h].blocks;
    r->lo = r->first == kNone ? 0 : blocks_[r->first].start;
    r->hi = r->first == kNone ? 0 : blocks_[r->first].start + blocks_[r->first].len;
    r->g.clear();
    if (!has_guests(h))
        return;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 g = order_[i];
        if (g != h && bodies_[g].alive && guest_host_[g] == h)
            r->g.push(g);
    }
}

void Soup::free_guests(const GuestRelease& r) {
    for (usize i = 0; i < r.g.size(); ++i)
        free_guest(r.g[i], r.first, r.lo, r.hi);
}

// S2 (Damian, 2026-11-09): a swallowed body isn't killed at once; each tick
// it loses engulf.digest codons from the end of its code (its first block,
// then its others) to its eater, appended to the eater's latest block when
// they lie next to it (the eater's stomach), else as a new latest block.
// When nothing of it is left its eater takes the rest of it (absorb).
void Soup::digest() {
    const u32 per = p_->digest;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 b = order_[i];
        if (!bodies_[b].alive || guest_kind_[b] != kGuestSwallowed)
            continue;
        const u32 eater = guest_host_[b];
        if (eater == kNone || !bodies_[eater].alive) { // (its eater died this tick: free)
            set_guest(b, kNone, kGuestNone);
            ++tally_.freed;
            continue;
        }
        const u32 g = genome_len(b);
        u32 left = per;
        while (left > 0 && bodies_[b].blocks != kNone) {
            // The last codon it owns of its last block (its first block last).
            u32 k = bodies_[b].blocks;
            while (blocks_[k].next != kNone)
                k = blocks_[k].next;
            Block& bl = blocks_[k];
            u32 a = kNone;
            for (u32 x = bl.start + bl.len; x > bl.start; --x)
                if (owner_[x - 1] == k) {
                    a = x - 1;
                    break;
                }
            if (a == kNone) { // (an empty block: drop it)
                if (bodies_[b].blocks == k) {
                    bodies_[b].blocks = kNone;
                } else {
                    u32 t = bodies_[b].blocks;
                    while (blocks_[t].next != k)
                        t = blocks_[t].next;
                    blocks_[t].next = kNone;
                }
                if (bodies_[b].latest == k)
                    bodies_[b].latest = kNone;
                forget_block(b, k);
                free_block(k);
                continue;
            }
            // Out of the prey ...
            owner_[a] = kNone;
            --bl.owned;
            --bodies_[b].mem;
            while (bl.len > 0 && owner_[bl.start + bl.len - 1] != k)
                --bl.len;
            // ... into the eater: its stomach (the block digestion last gave
            // it) grows by it if adjacent, else it is a new stomach. Neither
            // its code nor its latest block (a daughter in the making) changes.
            u32 sk = stomach_[eater];
            if (sk != kNone && (blocks_[sk].body != eater || sk == bodies_[eater].blocks || sk == bodies_[eater].latest ||
                                blocks_[sk].owned != blocks_[sk].len))
                sk = kNone;
            const bool below = sk != kNone && blocks_[sk].start == a + 1;
            const bool above = sk != kNone && blocks_[sk].start + blocks_[sk].len == a;
            if (below || above) {
                Block& L = blocks_[sk];
                if (below)
                    --L.start;
                ++L.len;
                ++L.owned;
                owner_[a] = sk;
                ++bodies_[eater].mem;
            } else {
                used_[a >> 6] &= ~(1ull << (a & 63)); // (new_block claims it back)
                ++waste_;
                stomach_[eater] = new_block(a, 1, eater);
            }
            if (!stamp_.empty()) // the prey's code, as a bite piece is (salvage)
                stamp_[a] = g;
            ++free_epoch_;
            ++tally_.digested;
            --left;
        }
        if (bodies_[b].blocks == kNone || bodies_[b].mem == 0)
            absorb(b, eater); // nothing of it left: it is food
    }
}

// Bleeding (B6, Damian): an end of a body's code whose outermost codon isn't a
// NOP loses that codon to waste, stamped as the body's remnant (food), until a
// NOP is outermost: written there by the program (a repair), or reached inside
// (an internal NOP is the new skin). A body bleeds on the ticks where tick +
// 1 + serial is a multiple of life.bleed_every, so no draws are made, bodies'
// turns are spread over the interval, and the first body's first turn is the
// interval's last tick. Resting dormant programs bleed too (they lie in
// memory); drifting ones are in transit.
void Soup::bleed() {
    const u64 every = p_->bleed_every;
    for (usize i = 0; i < order_.size(); ++i) {
        const u32 b = order_[i];
        Body& B = bodies_[b];
        if (!B.alive || B.blocks == kNone || (tick_ + 1 + B.serial) % every != 0)
            continue;
        if (guest_kind_[b] != kGuestNone) // (inside a host: a swallowed body is digested instead, S3;
            continue;                     //  a parasite's code is its host's and bleeds with it, M2)
        const u32 k = B.blocks;
        for (u32 side = 0; side < 2; ++side) {
            const u32 a = outer_codon(b, side == 1);
            if (a == kNone || is_nop(p_->table.op[mem_[a]]))
                continue;
            Block& bl = blocks_[k];
            if (!stamp_.empty() && stamp_[a] == 0)
                stamp_[a] = genome_len(b);
            owner_[a] = kNone;
            used_[a >> 6] &= ~(1ull << (a & 63));
            ++waste_;
            ++free_epoch_;
            --bl.owned;
            --B.mem;
            while (bl.len > 0 && owner_[bl.start] != k) { // the range stays tight around what it owns
                ++bl.start;
                --bl.len;
            }
            while (bl.len > 0 && owner_[bl.start + bl.len - 1] != k)
                --bl.len;
            ++tally_.bled;
        }
    }
}

// A varied world (docs/world-plan.md W5): sparks follow node activity.
double Soup::occupancy() const {
    u64 held = p_->size - waste_; // every codon a body owns ...
    if (p_->dormancy)             // ... but a resting dormant program's
        for (usize k = 0; k < order_.size(); ++k)
            if (dorm_[order_[k]].on)
                held -= bodies_[order_[k]].mem;
    return p_->size ? static_cast<double>(held) / p_->size : 0.0;
}

// Memory tiers (docs/world-plan.md W3): fast codons owned by anyone, from the
// bitmap (fast_end is whole pages, so whole words but at share 1).
u32 Soup::fast_held() const {
    const u32 fe = min_of(p_->fast_end, p_->size);
    u32 n = 0;
    for (u32 i = 0; i < (fe >> 6); ++i)
        n += static_cast<u32>(__popcnt64(used_[i]));
    for (u32 a = fe & ~63u; a < fe; ++a)
        n += owner_[a] != kNone ? 1u : 0u;
    return n;
}

double Soup::spark_rate_now() const {
    const double rate = p_->spark_rate;
    if (p_->spark_activity <= 0 || rate <= 0) // spark.activity 0: by time alone, exactly as before
        return rate;
    const double room = 1.0 - occupancy();
    return room > 0 ? rate * pow(room, p_->spark_activity) : 0.0;
}

// ---------------------------------------------------------------------------
// CPU units (the millicore ledger)
// ---------------------------------------------------------------------------

// Phase 9c: an E millicore goes to and from the E pool (is_e is false for
// every unit without them).
void Soup::pool_push(u32 u) {
    Array<u32>& pool = is_e(u) ? epool_ : pool_;
    unit_owner_[u] = kNone;
    pool_pos_[u] = static_cast<u32>(pool.size());
    pool.push(u);
}

void Soup::pool_remove(u32 u) {
    Array<u32>& pool = is_e(u) ? epool_ : pool_;
    u32 i = pool_pos_[u];
    u32 last = pool.back();
    pool[i] = last;
    pool_pos_[last] = i;
    pool.pop();
    pool_pos_[u] = kNone;
}

void Soup::append_unit(u32 body, u32 u) {
    Body& B = bodies_[body];
    if (is_e(u))
        ++eunits_[body];
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
    if (is_e(u))
        --eunits_[unit_owner_[u]];
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

// Phase 9c: E millicores, as claim_units draws P.
u32 Soup::claim_e_units(u32 body, u32 k) {
    if (k > epool_.size())
        k = static_cast<u32>(epool_.size());
    for (u32 i = 0; i < k; ++i) {
        u32 u = epool_[rng_.below(epool_.size())];
        pool_remove(u);
        append_unit(body, u);
    }
    return k;
}

// Up to k units from both pools in proportion to what each holds free (P
// first, then E); without E millicores, exactly claim_units.
u32 Soup::claim_mixed(u32 body, u32 k) {
    if (!ek())
        return claim_units(body, k);
    const u64 fp = pool_.size(), fe = epool_.size(), f = fp + fe;
    if (f == 0)
        return 0;
    const u64 t = k < f ? k : f;
    u64 ke = (2 * t * fe + f) / (2 * f); // t x fe / f, rounded
    if (ke > fe)
        ke = fe;
    if (t - ke > fp)
        ke = t - fp;
    return claim_units(body, static_cast<u32>(t - ke)) + claim_e_units(body, static_cast<u32>(ke));
}

// The oldest k units of one kind, to another body or into their pool.
void Soup::move_kind(u32 from, u32 to, u32 k, bool e) {
    if (!ek()) {
        if (!e)
            move_units(from, to, k);
        return;
    }
    u32 u = bodies_[from].unit_head;
    for (u32 i = 0; i < k && u != kNone;) {
        const u32 next = unit_next_[u];
        if (is_e(u) == e) {
            unlink_unit(u);
            append_unit(to, u);
            ++i;
        }
        u = next;
    }
}

void Soup::release_kind(u32 body, u32 k, bool e) {
    if (!ek()) {
        if (!e)
            release_units(body, k);
        return;
    }
    u32 u = bodies_[body].unit_head;
    for (u32 i = 0; i < k && u != kNone;) {
        const u32 next = unit_next_[u];
        if (is_e(u) == e) {
            unlink_unit(u);
            pool_push(u);
            ++i;
        }
        u = next;
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
// (per_unit is the substrate's: the setting x its speed, docs/world-plan.md W1.)
void UnitLedger::prepare(const Array<u32>& order, const HeatCurve& curve, bool heat_on, float per_unit) {
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
        cap[b] *= per_unit;
    }
}

// Each unit gains gain x its owner's utilisation (work done / nominal
// capacity) and cools like everything else; past max it is freed, hot.
u32 UnitLedger::heat_update(const HeatCurve& curve, float gain, float decay, float per_unit, float ambient) {
    u32 freed = 0;
    for (u32 u = 0; u < units(); ++u) {
        const u32 o = owner[u];
        float input = 0.0f;
        if (o != kNone) {
            const float nominal = static_cast<float>(count[o]) * per_unit;
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

// A varied world (docs/world-plan.md W1): a unit's work a tick is the
// setting x the substrate's speed (its warps' work follows: warp_pack).
void Soup::prepare_gpu() {
    if (p_->gpus)
        gpu_.prepare(order_, p_->gpu, p_->heat_on, static_cast<float>(p_->gpu_elements) * p_->gpu_speed);
    if (p_->npus)
        npu_.prepare(order_, p_->npu, p_->heat_on, static_cast<float>(p_->npu_macs) * p_->npu_speed);
}

void Soup::accel_heat_update() {
    if (!p_->heat_on)
        return;
    const float decay = 1.0f - p_->cool;
    for (usize i = 0; i < fever_unit_.size(); ++i) // disasters X10: fevered GPU units
        if ((fever_unit_[i] & kOfflineGpu) && (fever_unit_[i] & ~kOfflineGpu) < gpu_.units())
            gpu_.heat[fever_unit_[i] & ~kOfflineGpu] += fever_heat_[i];
    if (p_->gpus)
        tally_.gpu_released += gpu_.heat_update(p_->gpu, p_->gpu_gain, decay,
                                                static_cast<float>(p_->gpu_elements) * p_->gpu_speed, ambient_);
    if (p_->npus)
        tally_.npu_released += npu_.heat_update(p_->npu, p_->npu_gain, decay,
                                                static_cast<float>(p_->npu_macs) * p_->npu_speed, ambient_);
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

void DiskLedger::init(u32 n, Rng& rng, u32 fast_n) {
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
    fast = fast_n < n ? fast_n : n; // W4: [fast, n) is the archive
    afree = n - fast;
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

// First fit in [lo, hi) from `from`, then from lo (W4: one tier's range; the
// whole ledger without an archive, exactly as before).
u32 DiskLedger::find_run(u32 from, u32 len, u32 lo, u32 hi) const {
    u32 at = free_run_fwd(used.data(), hi, from, len);
    if (at == kNone && from > lo)
        at = free_run_fwd(used.data(), hi, lo, len);
    return at;
}

u32 DiskLedger::new_slot() {
    if (!free_ext.empty()) {
        const u32 x = free_ext.back();
        free_ext.pop();
        return x;
    }
    ext.push(DiskExtent{});
    return static_cast<u32>(ext.size() - 1);
}

u32 DiskLedger::add_extent(u32 body, u32 start, u32 len) {
    const u32 x = new_slot();
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
    if (start >= fast) // W4: an archive extent (never without an archive)
        afree -= len;
    return x;
}

// W4: each tier is its own range, [0, fast) or [fast, size); without an
// archive the fast disk is the whole ledger, so every draw is as before.
u32 DiskLedger::alloc_run(Rng& rng, u32 body, u32 len, bool arch) {
    const u32 lo = arch ? fast : 0, hi = arch ? size() : fast;
    if (len == 0 || len > (arch ? afree : free - afree))
        return kNone;
    const u32 at = find_run(lo + static_cast<u32>(rng.below(hi - lo)), len, lo, hi);
    if (at == kNone)
        return kNone;
    const u32 off = total[body];
    add_extent(body, at, len);
    return off;
}

u32 DiskLedger::claim_any(Rng& rng, u32 body, u32 count, bool arch) {
    const u32 lo = arch ? fast : 0, hi = arch ? size() : fast, n = hi - lo; // W4: one tier
    if (count == 0 || (arch ? afree : free - afree) == 0 || n == 0)
        return 0;
    u32 need = count, a = lo + static_cast<u32>(rng.below(n)), scanned = 0;
    while (need > 0 && scanned < n) {
        if ((used[a >> 6] >> (a & 63)) & 1) {
            ++a;
            ++scanned;
        } else {
            const u32 st = a;
            u32 len = 0;
            while (len < need && a < hi && !((used[a >> 6] >> (a & 63)) & 1)) {
                ++len;
                ++a;
            }
            add_extent(body, st, len);
            need -= len;
            scanned += len;
        }
        if (a == hi)
            a = lo;
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
        const u32 y = new_slot();
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

// Frees a detached chain: contents stay (fossils); with stamps, codons with
// stamp 0 take st.
void DiskLedger::free_chain(u32 x, u32 st) {
    while (x != kNone) {
        DiskExtent& X = ext[x];
        for (u32 a = X.start; a < X.start + X.len; ++a) { // contents stay: fossils
            owner[a] = kNone;
            used[a >> 6] &= ~(1ull << (a & 63));
            if (!stamp.empty() && stamp[a] == 0)
                stamp[a] = st;
        }
        free += X.len;
        if (X.start >= fast) // W4: archive codons (never without an archive)
            afree += X.len;
        const u32 next = X.next;
        X = DiskExtent{0, 0, kNone, kNone};
        free_ext.push(x);
        x = next;
    }
}

// Appends a detached chain to body `to`'s extents, in order.
void DiskLedger::append_chain(u32 to, u32 x) {
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

void DiskLedger::release_tail(u32 body, u32 k, u32 st) { free_chain(split_tail(body, k), st); }

void DiskLedger::move_tail(u32 from, u32 to, u32 k) { append_chain(to, split_tail(from, k)); }

void DiskLedger::move_all(u32 from, u32 to) { move_tail(from, to, total[from]); }

// --- disk tiers (docs/world-plan.md W4) --------------------------------------

u32 DiskLedger::held_in(u32 body, bool arch) const {
    u32 n = 0;
    for (u32 x = head[body]; x != kNone; x = ext[x].next)
        if ((ext[x].start >= fast) == arch)
            n += ext[x].len;
    return n;
}

bool DiskLedger::archive_in(u32 body, u32 off, u32 n, u32* arch) const {
    *arch = 0;
    if (n == 0 || static_cast<u64>(off) + n > total[body])
        return false;
    u32 x = head[body];
    while (off >= ext[x].len) {
        off -= ext[x].len;
        x = ext[x].next;
    }
    u32 left = n;
    while (left > 0 && x != kNone) {
        const u32 take = min_of(left, ext[x].len - off);
        if (ext[x].start >= fast)
            *arch += take;
        left -= take;
        off = 0;
        x = ext[x].next;
    }
    return left == 0;
}

// Detaches the body's newest kf fast and ka archive codons (each capped by
// what it holds there), splitting at most one extent of each tier, and
// returns the detached chain; both what stays and what goes keep their
// claim order.
u32 DiskLedger::split_tiers(u32 body, u32 kf, u32 ka) {
    const u32 ha = held_in(body, true), hf = total[body] - ha;
    kf = min_of(kf, hf);
    ka = min_of(ka, ha);
    if (kf + ka == 0)
        return kNone;
    const u32 keep_f = hf - kf, keep_a = ha - ka;
    u32 seen_f = 0, seen_a = 0; // codons of each tier passed so far
    u32 kept_tail = kNone, gone_head = kNone, gone_tail = kNone;
    u32 x = head[body];
    head[body] = kNone;
    auto keep = [&](u32 e) {
        if (kept_tail == kNone)
            head[body] = e;
        else
            ext[kept_tail].next = e;
        kept_tail = e;
    };
    auto drop = [&](u32 e) {
        if (gone_tail == kNone)
            gone_head = e;
        else
            ext[gone_tail].next = e;
        gone_tail = e;
    };
    while (x != kNone) {
        const u32 next = ext[x].next;
        ext[x].next = kNone;
        const bool arch = ext[x].start >= fast;
        u32& seen = arch ? seen_a : seen_f;
        const u32 limit = arch ? keep_a : keep_f, len = ext[x].len;
        const u32 stay = seen >= limit ? 0 : min_of(len, limit - seen);
        seen += len;
        if (stay == len) {
            keep(x);
        } else if (stay == 0) {
            drop(x);
        } else { // split x: its first part stays
            const u32 y = new_slot();
            ext[y] = DiskExtent{ext[x].start + stay, len - stay, body, kNone};
            ext[x].len = stay;
            for (u32 a = ext[y].start; a < ext[y].start + ext[y].len; ++a)
                owner[a] = y;
            keep(x);
            drop(y);
        }
        x = next;
    }
    tail[body] = kept_tail;
    total[body] = keep_f + keep_a;
    return gone_head;
}

void DiskLedger::release_tiers(u32 body, u32 kf, u32 ka, u32 st) { free_chain(split_tiers(body, kf, ka), st); }

void DiskLedger::move_tiers(u32 from, u32 to, u32 kf, u32 ka) { append_chain(to, split_tiers(from, kf, ka)); }

// --- end disk tiers ------------------------------------------------------------

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

// Phase 10: free disk codons (fossils) decay like memory's dead matter.
u32 DiskLedger::decay(Rng& rng, double rate) { return decay_range(rng, rate, 0, size()); }

// W4: over [lo, hi) only (one tier).
u32 DiskLedger::decay_range(Rng& rng, double rate, u32 lo, u32 hi) {
    u32 n = 0;
    const double size_d = static_cast<double>(hi - lo);
    double pos = -log(1.0 - rng.unit()) / rate;
    while (pos < size_d) {
        const u32 a = lo + static_cast<u32>(pos);
        if (owner[a] == kNone && mem[a]) {
            mem[a] = fade_bit(mem[a], rng);
            ++n;
        }
        pos += -log(1.0 - rng.unit()) / rate;
    }
    return n;
}

// Slow bit rot over the whole disk, owned or not, sampled like memory's.
u32 DiskLedger::rot(Rng& rng, double rate) { return rot_range(rng, rate, 0, size()); }

// W4: over [lo, hi) only (one tier).
u32 DiskLedger::rot_range(Rng& rng, double rate, u32 lo, u32 hi) {
    u32 flips = 0;
    const double n = static_cast<double>(hi - lo);
    double pos = -log(1.0 - rng.unit()) / rate;
    while (pos < n) {
        mem[lo + static_cast<u32>(pos)] ^= static_cast<u8>(1u << rng.below(6));
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
            if (ext[x].start < fast && ext[x].start + ext[x].len > fast) // W4: one tier each
                return fail("extent straddles the archive's boundary at extent", x);
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
    // W4: the archive's free codons counted (none without one).
    u32 af = 0;
    for (u32 a = fast; a < size(); ++a)
        af += owner[a] == kNone ? 1u : 0u;
    if (fast > size() || af != afree)
        return fail("archive free count wrong, actual", af);
    return true;
}

// Disk failure on one tier (docs/disasters-plan.md X5, amended by W4): the
// tier's holdings are released (in schedule order), resting dormant programs
// lose that tier's reserve, and every codon of it becomes INERT, stamps
// cleared. Draws nothing.
u32 Soup::fail_disk_tier(bool archive) {
    if (!disk_on())
        return 0;
    const u32 lo = archive ? disk_.fast : 0, hi = archive ? disk_.size() : disk_.fast;
    if (lo == hi)
        return 0;
    for (usize k = 0; k < order_.size(); ++k) {
        const u32 b = order_[k];
        if (!bodies_[b].alive)
            continue;
        const u32 held = disk_.tiered() ? disk_.held_in(b, archive) : disk_.total[b];
        if (held && disk_.tiered())
            disk_.release_tiers(b, archive ? 0 : held, archive ? held : 0);
        else if (held)
            disk_.release_tail(b, held);
        if (dormant(b))
            (archive ? dorm_[b].archive_reserve : dorm_[b].disk_reserve) = 0;
    }
    for (u32 a = lo; a < hi; ++a)
        disk_.mem[a] = 0;
    if (!disk_.stamp.empty())
        for (u32 a = lo; a < hi; ++a)
            disk_.stamp[a] = 0;
    return hi - lo;
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
    // Disasters X10: an OS crash's instability, spurious faults per
    // instruction (drawn only while it brews, so other ticks are unchanged).
    faults_on_ = weather_.faults > 0;
    fault_left_ = faults_on_ ? -log(1.0 - rng_.unit()) / weather_.faults : 1e300;
}

// The end of a tick (Phase 4 lifecycle): whether the node is due to collapse,
// worn out. (A full node no longer collapses for being full: the starvation
// rule is gone, docs/dormancy-plan.md D7.)
void Soup::lifecycle_check() {
    collapse_ = p_->endurance > 0 && static_cast<double>(writes_) >= p_->endurance;
}

u32 Soup::health() const {
    double h = 1.0;
    if (p_->endurance > 0) {
        const double w = static_cast<double>(writes_) / p_->endurance;
        h = min_of(h, 1.0 - w);
    }
    h = h < 0 ? 0 : h;
    return static_cast<u32>(h * 100.0);
}

// Every body dies an ordinary death: its code stays behind as remnants
// (stamped, with salvage on) and its disk as fossils; units go to the pools.
void Soup::collapse() {
    repair(~0ull); // disasters: offline cores are moot on a fallow node (back to the pools)
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
    u32 n = 0;
    u64 in_n = 0, addr_n = 0;
    accel_gather_size(&n, &in_n, &addr_n);
    const usize o = ops->size(), i0 = in->size(), a0 = addr->size();
    ops->resize(o + n);
    in->resize(i0 + static_cast<usize>(in_n));
    addr->resize(a0 + static_cast<usize>(addr_n));
    accel_gather_at(ops->data() + o, in->data(), static_cast<u32>(i0), addr->data(), static_cast<u32>(a0));
}

// The gather in two halves, so the world's nodes can gather at once, each
// into its own stretch of the batch: what this node's issued ops take...
void Soup::accel_gather_size(u32* ops, u64* in, u64* addr) const {
    u64 r = 0, w = 0;
    for (usize i = 0; i < issued_.size(); ++i) {
        r += issued_[i].src_n + issued_[i].dst_n + issued_[i].mat_n;
        w += issued_[i].out_n;
    }
    *ops = static_cast<u32>(issued_.size());
    *in = r;
    *addr = w;
}

// ... and the gather into it: ops[0..] and the batch's in and addr arrays
// from in_off and addr_off (the ops' offsets are into those whole arrays).
void Soup::accel_gather_at(AccelOp* ops, u8* in, u32 in_off, u32* addr, u32 addr_off) {
    for (usize i = 0; i < issued_.size(); ++i) {
        AccelOp op = issued_[i];
        const u32* a = issue_addr_.data() + issue_ref_[i];
        const u32 reads = op.src_n + op.dst_n + op.mat_n;
        op.in_off = in_off;
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
            in[in_off++] = v;
        }
        op.addr_off = addr_off;
        if (op.out_n)
            memcpy(addr + addr_off, a + reads, static_cast<usize>(op.out_n) * 4);
        addr_off += op.out_n;
        ops[i] = op;
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
        if (a >= p_->size || !mine(op.body, a)) // (a parasite's: its host's too, inject-plan M2)
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
    out->disk_fast = disk_.fast; // W4: where its archive began
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
    const double t0 = timing_on_ ? clock_seconds() : 0; // (profiling)
    double t_bodies = 0;
    begin_tick(tick);
    if (!vent_at_.empty()) // Phase 10: fresh material before anything claims it
        vents();

    const double rate = spark_rate_now(); // W5: fading as the node fills (spark.activity)
    u32 count = static_cast<u32>(rate);
    if (rng_.chance(rate - count))
        ++count;
    // spark.timing (D10): "first" (Phase 1a) lets chemistry claim freed CPU
    // before life does; "last" leaves it only what the bodies did not claim.
    if (!p_->spark_late)
        sparks(count);

    prepare_heat();
    prepare_gpu();
    if (prof_) // behaviour profiles (A3): a round's picks, from the observer's own stream (observe-only)
        prof_->on_tick_start(*this, tick);

    // Fixed cyclic order (creation order) with a start rotated by the node RNG,
    // so no body has a permanent first-mover edge (7.1 D). Bodies born during
    // the tick are appended and first run next tick.
    const usize n = order_.size();
    sched_n_ = n;
    sched_start_ = 0;
    if (n > 0) {
        usize start = static_cast<usize>(rng_.below(n));
        sched_start_ = start;
        const double tb = timing_on_ ? clock_seconds() : 0;
        for (usize k = 0; k < n; ++k) {
            u32 b = order_[(start + k) % n];
            if (bodies_[b].alive && !dormant(b)) // (a resting dormant program doesn't run, nor pay upkeep)
                run_body(b, tick);
        }
        if (timing_on_)
            t_bodies = clock_seconds() - tb;
    }
    if (p_->spark_late)
        sparks(count);
    if (timing_on_) {
        timing_.bodies += t_bodies;
        timing_.first += clock_seconds() - t0 - t_bodies;
    }
}

// The tick's second half: heat, hazards and the death sweep.
void Soup::tick_second() {
    const double t0 = timing_on_ ? clock_seconds() : 0; // (profiling)
    tick_second_parts();
    if (timing_on_)
        timing_.second += clock_seconds() - t0;
}

void Soup::tick_second_parts() {
    heat_update();
    accel_heat_update();
    bit_rot();
    cpu_wear();
    accel_wear();
    // Phase 4 hazards, after every earlier draw so worlds without them are
    // unchanged.
    if (p_->disk && p_->disk_rot > 0) // (W4: the fast disk, [0, fast): without an archive, all of it)
        tally_.disk_rot += disk_.rot_range(rng_, p_->disk_rot, 0, disk_.fast);
    if (p_->volatility > 0 || !pending_wipes_.empty()) // (W: warned wipes land even if volatility is now 0)
        volatility();
    // Phase 10, after every earlier draw so worlds without it are unchanged.
    if (p_->mem_decay > 0)
        decay();
    if (p_->disk && p_->disk_decay > 0)
        tally_.decayed += disk_.decay_range(rng_, p_->disk_decay, 0, disk_.fast);
    // Disk tiers (W4): the archive's rot and fossil decay, after every
    // earlier draw so worlds without an archive are unchanged.
    if (disk_.tiered()) {
        if (p_->archive_rot > 0)
            tally_.disk_rot += disk_.rot_range(rng_, p_->archive_rot, disk_.fast, disk_.size());
        if (p_->archive_decay > 0)
            tally_.decayed += disk_.decay_range(rng_, p_->archive_decay, disk_.fast, disk_.size());
    }
    if (weather_.disk_rot > 0 || weather_.static_rot > 0) // disasters X10, after every earlier draw
        weather_hazards();
    if (p_->bleed_every > 0) // docs/bites-plan.md B6 (no draws)
        bleed();
    if (p_->digest > 0) // docs/inject-plan.md S2 (no draws)
        digest();
    death_sweep();
    if (!actions_.empty()) // Phase 9
        sweep_actions();
    if (p_->endurance > 0)
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

bool Soup::cpu_waits(u32 c) const {
    const u32 i = action_of(c);
    return i != kNone && !busy_action(actions_[i]) && actions_[i].end > tick_;
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
//
// Wipe warnings (docs/lifehistory-plan.md W2-W3): with mem.wipe_warning > 0 a
// wipe drawn now lands that many ticks later, and a share
// mem.wipe_false_alarms of them (drawn now) fade then instead; until then its
// pages heat and flicker (wipe_symptoms). The wipes due land first, in the
// order drawn, then this tick's are drawn. With no warning nothing is
// pending, and every draw is as before.
void Soup::volatility() {
    const u32 pages = static_cast<u32>(page_heat_.size());
    bool landed = false;
    auto begin = [&] {
        if (landed)
            return;
        wiped_pg_.resize(pages);
        wiped_pg_.fill(0);
        wiped_bodies_.clear();
        landed = true;
    };
    if (!pending_wipes_.empty()) {
        usize w = 0;
        for (usize i = 0; i < pending_wipes_.size(); ++i) {
            const PendingWipe x = pending_wipes_[i];
            if (x.lands > tick_) {
                pending_wipes_[w++] = x;
                continue;
            }
            if (!x.false_alarm) {
                begin();
                land_wipe(x.page, x.pages);
            }
        }
        pending_wipes_.resize(w);
    }
    const double rate = p_->volatility * hazard_mult_;
    if (rate > 0) {
        // Memory tiers (docs/world-plan.md W3): a fast page is wiped
        // mem.fast_volatility times as often. The process runs over the pages
        // weighted by their chance: the first fp pages (fast) fv each, the rest
        // 1. Without tiers fp is 0, and every draw and page is as before.
        const u32 fp = min_of<u32>((p_->fast_end + kPage - 1) >> kPageBits, pages);
        const double fv = p_->fast_volatility, fw = static_cast<double>(fp) * fv;
        const double span = fw + static_cast<double>(pages - fp);
        double pos = -log(1.0 - rng_.unit()) / rate;
        while (pos < span) {
            u32 pg = pos < fw ? static_cast<u32>(pos / fv) : fp + static_cast<u32>(pos - fw);
            if (pg >= pages) // (rounding at the very end)
                pg = pages - 1;
            const u32 len = 1 + static_cast<u32>(rng_.below(p_->wipe_pages));
            if (p_->wipe_warning > 0) {
                const u32 fa = rng_.chance(p_->wipe_false_alarms) ? 1u : 0u;
                pending_wipes_.push(PendingWipe{tick_, tick_ + p_->wipe_warning, pg, len, fa, 0});
                ++tally_.wipe_warnings;
                tally_.wipe_false_alarms += fa;
            } else {
                begin();
                land_wipe(pg, len);
            }
            pos += -log(1.0 - rng_.unit()) / rate;
        }
    }
    if (!pending_wipes_.empty())
        wipe_symptoms();
    if (!landed)
        return;
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

void Soup::land_wipe(u32 pg, u32 len) {
    const u32 pages = static_cast<u32>(page_heat_.size());
    const u32 start = pg << kPageBits;
    const u32 end = static_cast<u32>(min_of<u64>(static_cast<u64>(pg + len) << kPageBits, p_->size));
    if (start >= end)
        return;
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
}

// docs/lifehistory-plan.md W2: each pending wipe's pages, f = (tick - drawn) /
// mem.wipe_warning of the way to it: their heat raised to at least f x the
// memory curve's hot mark (with heat on: SENSE reads it, and the error
// multiplier rises with it), and bit flips at f x mem.wipe_flicker per codon
// (a Poisson process over the wipe's codons, from the node's stream). False
// alarms show the same; nothing else does, so a symptom is a strong hint.
void Soup::wipe_symptoms() {
    const double W = static_cast<double>(p_->wipe_warning);
    if (!(W > 0))
        return;
    const u32 pages = static_cast<u32>(page_heat_.size());
    for (usize i = 0; i < pending_wipes_.size(); ++i) {
        const PendingWipe& x = pending_wipes_[i];
        const double f = static_cast<double>(tick_ - x.drawn) / W;
        const u32 start = x.page << kPageBits;
        const u32 end = static_cast<u32>(min_of<u64>(static_cast<u64>(x.page + x.pages) << kPageBits, p_->size));
        if (!(f > 0) || start >= end)
            continue;
        if (p_->heat_on) {
            const float h = static_cast<float>(f) * p_->mem.hot;
            for (u32 q = x.page; q < pages && (q << kPageBits) < end; ++q)
                if (page_heat_[q] < h)
                    page_heat_[q] = h;
        }
        const double rate = f * p_->wipe_flicker;
        if (!(rate > 0))
            continue;
        double pos = static_cast<double>(start) - log(1.0 - rng_.unit()) / rate;
        while (pos < static_cast<double>(end)) {
            mem_[static_cast<u32>(pos)] ^= static_cast<u8>(1u << rng_.below(6));
            ++tally_.wipe_flickers;
            pos += -log(1.0 - rng_.unit()) / rate;
        }
    }
}

void Soup::sparks(u32 count) {
    // Live knob changes may leave min above max for a while; use the range.
    const u32 lo = min_of(p_->spark_min, p_->spark_max);
    const u32 hi = max_of(p_->spark_min, p_->spark_max);
    for (u32 i = 0; i < count; ++i) {
        // Phase 8: with warps a spark may draw GPU units instead (or as well).
        const bool gpu_pool = p_->warps && p_->spark_gpu_grant && p_->gpus && !gpu_.pool.empty();
        if (pool_.empty() && epool_.empty() && !gpu_pool)
            return; // a spark only draws from the free pools
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
            const u32 b = create_body(at, len, p_->spark_grant);
            if (b != kNone) // (always: the run is waste)
                origin_[b] = kOriginSpark;
        } else {
            // Its form: whichever it holds; holding both, the GPU with
            // chance spark.gpu_born.
            const u32 b = new_body();
            origin_[b] = kOriginSpark;
            new_block(at, len, b);
            claim_mixed(b, p_->spark_grant);
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
    if (ek()) { // Phase 9c: E units' speed apart too; nothing done on them yet
        for (usize k = 0; k < order_.size(); ++k) {
            espeed_[order_[k]] = 0.0f;
            ework_[order_[k]] = 0;
        }
    }
    for (u32 u = 0; u < m; ++u) {
        u32 o = unit_owner_[u];
        if (o == kNone)
            continue;
        Body& B = bodies_[o];
        float h = unit_heat_[u];
        const float sp = p_->heat_on ? p_->cpu.speed(h) : 1.0f;
        B.speed += sp;
        if (is_e(u))
            espeed_[o] += sp;
        B.err += h;
    }
    for (usize k = 0; k < order_.size(); ++k) {
        Body& B = bodies_[order_[k]];
        B.err = (p_->heat_on && B.units) ? p_->cpu.err_mult(B.err / B.units) : 1.0f;
    }
    // A varied world (docs/world-plan.md W1): the substrate's P and E cores
    // run their kind's speed x a tick's time (what the body's time comes from;
    // heat and errors stay the units' own).
    if (p_->p_speed != 1.0f || p_->e_speed != 1.0f) {
        const bool e_on = ek();
        for (usize k = 0; k < order_.size(); ++k) {
            const u32 b = order_[k];
            const float e = e_on ? espeed_[b] : 0.0f;
            bodies_[b].speed = (bodies_[b].speed - e) * p_->p_speed + e * p_->e_speed;
            if (e_on)
                espeed_[b] = e * p_->e_speed;
        }
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
    // Phase 9c: the share of its time on E millicores (0 without them).
    const double ef = ek() && bodies_[b].speed > 0 ? static_cast<double>(espeed_[b]) / bodies_[b].speed : 0.0;
    auto add_ework = [&](double t) {
        const double v = static_cast<double>(ework_[b]) + t * ef;
        ework_[b] = v > 4e9 ? 4000000000u : static_cast<u32>(v);
    };
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
        if (ef > 0) // paid on both kinds, in proportion
            add_ework(time < 4e9 ? time : 4e9);
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
                if (ef > 0)
                    add_ework(cap < 4e9 ? cap : 4e9);
            }
        }
        return;
    }

    // The body's share splits equally among its running CPUs; the remainder
    // goes one each to CPUs from a tick-rotated offset.
    // Phase 9c: a CPU waiting on the disk or a device is idle -- its units
    // cool -- and its share goes to the body's other CPUs, as an OS runs
    // another thread while one waits. (Its action lands at its own turn in
    // its end tick, when it is no longer waiting.)
    scratch_.clear();
    for (u32 c = bodies_[b].cpus; c != kNone; c = cpus_[c].next) {
        if (!cpus_[c].halted) {
            if (!actions_.empty() && cpu_waits(c))
                continue;
            cpus_[c].yielded = 0;
            scratch_.push(c);
        }
    }
    const usize k = scratch_.size();
    if (k == 0)
        return;
    // scratch_ is a snapshot: CPUs an ENGULF adds run from next tick.
    if (!p_->threads && !ek()) {
        const i64 q = n / static_cast<i64>(k);
        const usize extra = static_cast<usize>(n % static_cast<i64>(k));
        const usize off = static_cast<usize>(tick % k);
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
        return;
    }
    // Phase 9c, threads and cores (design.md 6.15). With threads a thread
    // runs on one core at a time, so it gets at most a core's tick; what its
    // body holds beyond that, with too few threads to use it, goes unused and
    // cools. With E millicores its time is P time and E time, handed out
    // first to the threads preferring each kind, then as leftovers to threads
    // still short of a core (share_time); a thread runs its preferred kind's
    // time first, every instruction at the costs of the kind it runs on.
    // Without threads one thread may use all its body's time, of both kinds.
    // The threads take turns, cpu.slices times a tick, each running a slice's
    // worth of its time per turn, in the body's thread order rotated each
    // tick: side by side in world time, so they can hand data to one another
    // within a tick. (One thread runs straight through: the same.)
    i64 te = ef > 0 ? static_cast<i64>(floor(static_cast<double>(n) * ef)) : 0;
    if (te > n)
        te = n;
    i64 cap = p_->threads ? static_cast<i64>(p_->tick_units) : 0x3FFFFFFFFFFFFFFFll;
    i64 cap_e = cap;
    if (p_->threads && (p_->p_speed != 1.0f || p_->e_speed != 1.0f)) { // W1: a core gives its kind's speed x a tick
        cap = static_cast<i64>(p_->tick_units * p_->p_speed);
        cap_e = static_cast<i64>(p_->tick_units * p_->e_speed);
    }
    share_time(k, n - te, te, cap, cap_e, tick);
    const usize off = static_cast<usize>(tick % k);
    const u32 slices = p_->threads && k > 1 ? p_->slices : 1;
    for (u32 sl = 0; sl < slices; ++sl) {
        for (usize j = 0; j < k; ++j) {
            const usize i = (j + off) % k;
            if (tq_[i] <= 0)
                continue;
            const i64 allow = tq_[i] * static_cast<i64>(sl + 1) / static_cast<i64>(slices);
            const u32 c = scratch_[i];
            const bool pe = cpu_kind(c) == kKindE;
            const i64 pref = pe ? tqe_[i] : tq_[i] - tqe_[i]; // its time on its preferred kind
            while (tu_[i] < allow) {
                const i64 done = pe ? tue_[i] : tu_[i] - tue_[i];
                const bool on_pref = done < pref;
                i64 quota = allow - tu_[i];
                if (on_pref && pref - done < quota)
                    quota = pref - done;
                const bool e = on_pref ? pe : !pe;
                const i64 used = run_cpu(c, quota, e ? kKindE : kKindP);
                tu_[i] += used;
                if (e)
                    tue_[i] += used;
                if (used < quota || !on_pref)
                    break; // it stopped, or ran on both kinds
            }
        }
    }
    u64 ew = 0;
    if (cpu_time_.size() < cpus_.size()) {
        cpu_time_.resize(cpus_.size());
        cpu_etime_.resize(cpus_.size());
    }
    for (u32 c = bodies_[b].cpus; c != kNone; c = cpus_[c].next)
        cpu_time_[c] = cpu_etime_[c] = 0;
    for (usize i = 0; i < k; ++i) {
        Body& B = bodies_[b];
        B.work += static_cast<u32>(tu_[i]);
        ew += static_cast<u64>(tue_[i]);
        cpu_time_[scratch_[i]] = tu_[i] > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<u32>(tu_[i]);
        cpu_etime_[scratch_[i]] = tue_[i] > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<u32>(tue_[i]);
        if (tu_[i] > tq_[i])
            B.carry -= static_cast<float>(tu_[i] - tq_[i]); // attack debt
    }
    if (ew > 0) {
        tally_.e_work += ew;
        const u64 v = ework_[b] + ew;
        ework_[b] = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(v);
    }
}

// Phase 9c (R3): a body's P time tp and E time te among its k running
// threads (scratch_), into tq_ (each thread's time) and tqe_ (its E part).
// First each thread gets up to `cap` (a core's tick; without threads, no
// limit) of its preferred kind, shared equally (the remainder one each from
// a tick-rotated offset); then what's left of each kind goes the same way to
// the other kind's threads still short of a core. A kind's threads are short
// only once that kind is used up, so no thread waits while its body has time
// of the other kind. Without E millicores and E-preferring threads: an equal
// split, capped (Phase 9c-b1).
// A varied world (docs/world-plan.md W1): where the substrate's P and E cores
// differ in speed, a core of P gives cap and one of E cap_e, and a thread
// with part of a core's time of one kind has the rest of a core left for the
// other (equal caps: exactly the rule above).
void Soup::share_time(usize k, i64 tp, i64 te, i64 cap, i64 cap_e, u64 tick) {
    tq_.resize(k);
    tqe_.resize(k);
    tu_.resize(k);
    tue_.resize(k);
    for (usize i = 0; i < k; ++i)
        tq_[i] = tqe_[i] = tu_[i] = tue_[i] = 0;
    // The time of kind e (E: true) thread i can still take: what's left of its core.
    auto room = [&](usize i, bool e) -> i64 {
        if (cap == cap_e)
            return cap - tq_[i];
        const double used = (cap > 0 ? static_cast<double>(tq_[i] - tqe_[i]) / static_cast<double>(cap) : 1.0) +
                            (cap_e > 0 ? static_cast<double>(tqe_[i]) / static_cast<double>(cap_e) : 1.0);
        return used < 1.0 ? static_cast<i64>((1.0 - used) * static_cast<double>(e ? cap_e : cap)) : 0;
    };
    if (k == 1) { // most bodies: the rule below, worked out for one thread
        const bool pe = cpu_kind(scratch_[0]) == kKindE;
        const i64 tpref = pe ? te : tp, toth = pe ? tp : te;
        const i64 cpref = pe ? cap_e : cap;
        const i64 a = tpref < cpref ? tpref : cpref;
        tq_[0] = a; // (what room() reads)
        tqe_[0] = pe ? a : 0;
        const i64 left = room(0, !pe);
        const i64 o = toth < left ? toth : left;
        tq_[0] = a + o;
        tqe_[0] = pe ? a : o;
        return;
    }
    auto hand_out =[&](i64 total, bool e) -> i64 { // to grp_; returns what's given
        const usize g = grp_.size();
        if (g == 0 || total <= 0)
            return 0;
        const i64 q = total / static_cast<i64>(g);
        const usize extra = static_cast<usize>(total % static_cast<i64>(g));
        const usize off = static_cast<usize>(tick % g);
        i64 given = 0;
        for (usize j = 0; j < g; ++j) {
            const usize i = grp_[j];
            const i64 share = q + (((j + g - off) % g) < extra ? 1 : 0);
            const i64 need = room(i, e);
            const i64 a = share < need ? share : need;
            tq_[i] += a;
            if (e)
                tqe_[i] += a;
            given += a;
        }
        return given;
    };
    i64 left[2] = {tp, te};
    for (u32 x = 0; x < 2; ++x) { // each kind to the threads preferring it
        grp_.clear();
        for (usize i = 0; i < k; ++i)
            if (cpu_kind(scratch_[i]) == x)
                grp_.push(static_cast<u32>(i));
        left[x] -= hand_out(left[x], x == kKindE);
    }
    for (u32 x = 0; x < 2; ++x) { // its leftovers to the other kind's threads short of a core
        grp_.clear();
        for (usize i = 0; i < k; ++i)
            if (cpu_kind(scratch_[i]) != x && room(i, x == kKindE) > 0)
                grp_.push(static_cast<u32>(i));
        hand_out(left[x], x == kKindE);
    }
}

void Soup::heat_update() {
    if (!p_->heat_on)
        return;
    const float decay = 1.0f - p_->cool;

    // CPU units: input from the owner's utilisation of its nominal share.
    // Phase 9c: with E millicores, P units from its work on P and E units from
    // its work on E, at heat.e_gain x the P gain (B.speed then holds P's input
    // and eheat_ E's).
    const bool ek_on = ek();
    for (usize k = 0; k < order_.size(); ++k) {
        const u32 b = order_[k];
        Body& B = bodies_[b];
        const u32 eu = ek_on ? eunits_[b] : 0;
        const u32 ew = ek_on ? ework_[b] : 0;
        // (W1: a unit's nominal work is its kind's speed x a tick, so a fast
        // core busy all tick heats as a busy core does)
        float nominal = static_cast<float>(B.units - eu) * static_cast<float>(p_->tick_units) / 1000.0f * p_->p_speed;
        float work = static_cast<float>(B.work - (ew < B.work ? ew : B.work));
        if (p_->cost.timed && work > nominal) // Phase 9: a tick's overshoot heats the ticks that pay it off
            work = nominal;
        B.speed = nominal > 0 ? p_->cpu_gain * work / nominal : 0.0f;
        if (ek_on) {
            const float enom = static_cast<float>(eu) * static_cast<float>(p_->tick_units) / 1000.0f * p_->e_speed;
            float ework = static_cast<float>(ew);
            if (ework > enom)
                ework = enom;
            eheat_[b] = enom > 0 ? p_->e_gain * p_->cpu_gain * ework / enom : 0.0f;
        }
    }
    const u32 m = p_->millicores;
    // The ambient leak (Phase 4, design.md 6.10): heat.ambient x the mean CPU
    // heat input over every millicore, held or free, reaches every unit and
    // page on the node, so one busy body warms everyone a little.
    ambient_ = 0.0f;
    if (p_->ambient > 0 && m > 0) {
        double sum = 0;
        for (usize k = 0; k < order_.size(); ++k) {
            const u32 b = order_[k];
            const Body& B = bodies_[b];
            if (ek_on)
                sum += static_cast<double>(B.speed) * (B.units - eunits_[b]) + static_cast<double>(eheat_[b]) * eunits_[b];
            else
                sum += static_cast<double>(B.speed) * B.units;
        }
        ambient_ = static_cast<float>(p_->ambient * sum / m);
    }
    // Disasters X10: a surge's brownout warms every unit and page here, and a
    // burnout's fevered cores run hotter than their work explains.
    ambient_ += weather_.heat;
    for (usize i = 0; i < fever_unit_.size(); ++i)
        if (!(fever_unit_[i] & kOfflineGpu) && fever_unit_[i] < m)
            unit_heat_[fever_unit_[i]] += fever_heat_[i];
    for (u32 u = 0; u < m; ++u) {
        u32 o = unit_owner_[u];
        float h = unit_heat_[u] * decay + (o != kNone ? (is_e(u) ? eheat_[o] : bodies_[o].speed) : 0.0f);
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
// Phase 10 (design.md 6.16): dead matter decays toward codon 0 (INERT in
// nova_v2). Each waste codon loses one random set bit with chance mem.decay a
// tick, sampled as a Poisson process over the soup like rot. Living code
// doesn't decay (cosmic rays, hazard.rot, still flip it).
void Soup::decay() {
    const double r = p_->mem_decay;
    const double n = static_cast<double>(p_->size);
    double pos = -log(1.0 - rng_.unit()) / r;
    while (pos < n) {
        const u32 a = static_cast<u32>(pos);
        if (owner_[a] == kNone && mem_[a]) {
            mem_[a] = fade_bit(mem_[a], rng_);
            ++tally_.decayed;
        }
        pos += -log(1.0 - rng_.unit()) / r;
    }
}

// Phase 10 (design.md 6.16): each vent writes vent.rate random codons a tick
// into waste within vent.radius of it, nearer more often (a triangular
// spread). Living code is skipped: vents spew material, not radiation.
// Fresh material is nobody's remnant.
void Soup::vents() {
    const u32 n = p_->size, r = p_->vent_radius < n ? p_->vent_radius : n - 1;
    for (usize v = 0; v < vent_at_.size(); ++v) {
        const i64 at = vent_at_[v];
        for (u32 i = 0; i < p_->vent_rate; ++i) {
            const i64 d = static_cast<i64>(rng_.below(r + 1)) + static_cast<i64>(rng_.below(r + 1)) - r;
            const u32 a = static_cast<u32>(((at + d) % n + n) % n);
            const u8 codon = static_cast<u8>(rng_.next() & 63);
            if (owner_[a] != kNone)
                continue;
            mem_[a] = codon;
            if (!stamp_.empty())
                stamp_[a] = 0;
            ++tally_.vent_codons;
        }
    }
}

// docs/lifehistory-plan.md R1 (see soup.h).
Soup::RayRun Soup::ray(u32 at, bool fwd, double energy, bool heat, bool wipe, Rng& r, bool indels) {
    const u32 n = p_->size;
    RayRun out{0, 0, 0};
    if (at >= n || !(energy > 0))
        return out;
    u32 a = at;
    if (fwd)
        while (a > 0 && owner_[a] != kNone)
            --a;
    else
        while (a + 1 < n && owner_[a] != kNone)
            ++a;
    const u32 from = a;
    const CodonTable& t = p_->table;
    double e = energy;
    for (;;) {
        const Op op = t.op[mem_[a]];
        const bool nop = is_nop(op);
        const bool dead = owner_[a] == kNone || op == Op::inert; // (what it passes, before the hit)
        if (wipe) {
            mem_[a] = static_cast<u8>(r.next() & 63);
            if (!stamp_.empty())
                stamp_[a] = 0;
            ++out.changed;
        } else {
            double c = nop ? p_->ray_flip_nop : p_->ray_flip;
            if (heat)
                c *= p_->mem.err_mult(page_heat_[a >> kPageBits]);
            if (c > 0 && r.unit() < c) {
                // R7: an energetic hit on living code may be an indel instead.
                bool done = false;
                if (indels && owner_[a] != kNone && e >= p_->ray_indel_energy && p_->ray_indel > 0 &&
                    r.unit() < p_->ray_indel) {
                    const bool insert = r.below(2) == 0;
                    done = ray_indel(a, insert);
                    if (done)
                        ++(insert ? tally_.ray_inserts : tally_.ray_deletes);
                }
                if (!done)
                    mem_[a] ^= static_cast<u8>(1u << r.below(6));
                ++out.changed;
            }
        }
        ++out.n;
        e -= nop ? p_->ray_absorb_nop : dead ? p_->ray_absorb_waste : p_->ray_absorb;
        if (!(e > 0))
            break;
        if (fwd) {
            if (++a >= n)
                break;
        } else {
            if (a == 0)
                break;
            --a;
        }
    }
    out.lo = fwd ? from : from + 1 - out.n;
    return out;
}

// R7 (see soup.h).
bool Soup::ray_indel(u32 a, bool insert) {
    if (a >= p_->size)
        return false;
    const u32 k = owner_[a];
    if (k == kNone)
        return false;
    Block& bl = blocks_[k];
    if (bl.owned != bl.len || (!insert && bl.len < 2))
        return false; // (a block with holes: a flip instead)
    const u32 s = bl.start, e = s + bl.len; // [s, e)
    const bool tail = e < p_->size && owner_[e] == kNone;
    const bool head = !tail && s > 0 && owner_[s - 1] == kNone;
    if (!tail && !head)
        return false; // no room at either end: a flip instead, for either kind
    Body& B = bodies_[bl.body];
    const bool st = !stamp_.empty();
    auto move = [&](u32 to, u32 from) {
        mem_[to] = mem_[from];
        if (st)
            stamp_[to] = stamp_[from];
    };
    auto claim = [&](u32 c) {
        owner_[c] = k;
        if (st)
            stamp_[c] = 0;
        mark_used(c, 1, true);
        --waste_;
        ++bl.len;
        ++bl.owned;
        ++B.mem;
    };
    auto release = [&](u32 c) { // as heat lets a codon go: a remnant of this body
        if (st && stamp_[c] == 0)
            stamp_[c] = genome_len(bl.body);
        owner_[c] = kNone;
        used_[c >> 6] &= ~(1ull << (c & 63));
        ++waste_;
        ++free_epoch_;
        --bl.len;
        --bl.owned;
        --B.mem;
    };
    u32 lo = 0, hi = 0; // the codons moved or changed: [lo, hi)
    if (insert && tail) { // a+1 .. e-1 up one, into e; a's copy at a+1
        claim(e);
        for (u32 i = e; i > a + 1; --i)
            move(i, i - 1);
        move(a + 1, a);
        lo = a;
        hi = e + 1;
    } else if (insert) { // s .. a down one, into s-1; a's copy stays at a
        claim(s - 1);
        bl.start = s - 1;
        for (u32 i = s - 1; i < a; ++i)
            move(i, i + 1);
        lo = s - 1;
        hi = a + 1;
    } else if (tail) { // a+1 .. e-1 down one; e-1 lets go
        for (u32 i = a; i + 1 < e; ++i)
            move(i, i + 1);
        release(e - 1);
        lo = a;
        hi = e;
    } else { // s .. a-1 up one; s lets go
        for (u32 i = a; i > s; --i)
            move(i, i - 1);
        release(s);
        bl.start = s + 1;
        lo = s;
        hi = a + 1;
    }
    if (obs_)
        obs_->on_wipe(lo, hi - lo);
    return true;
}

double Soup::ray_rate(double flip_rate, double energy) const {
    const double per = p_->ray_flip * ceil(energy / p_->ray_absorb);
    return flip_rate > 0 && per > 0 ? flip_rate / per : 0.0;
}

u64 Soup::ray_shower(double rate, double energy, bool heat, Rng& r, bool indels) {
    if (!(rate > 0))
        return 0;
    u64 flips = 0;
    const double n = static_cast<double>(p_->size);
    double pos = -log(1.0 - r.unit()) / rate;
    while (pos < n) {
        const bool fwd = r.below(2) == 0;
        flips += ray(static_cast<u32>(pos), fwd, energy, heat, false, r, indels).changed;
        pos += -log(1.0 - r.unit()) / rate;
    }
    return flips;
}

void Soup::bit_rot() {
    if (p_->rot_rate <= 0)
        return;
    if (p_->rays) { // docs/lifehistory-plan.md R2
        tally_.rot_flips += ray_shower(ray_rate(p_->rot_rate * hazard_mult_, p_->ray_cosmic), p_->ray_cosmic,
                                       p_->heat_on, rng_, true); // (R7: cosmic rays' indels)
        return;
    }
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

// Phase 10 (design.md 6.16): a halted thread leaves its body (and any action
// it had goes with it).
void Soup::end_thread(u32 b, u32 c) {
    Body& B = bodies_[b];
    if (B.cpus == c) {
        B.cpus = cpus_[c].next;
    } else {
        u32 t = B.cpus;
        while (cpus_[t].next != c)
            t = cpus_[t].next;
        cpus_[t].next = cpus_[c].next;
    }
    --B.ncpu;
    cpus_[c].body = kNone;
    cpus_[c].next = kNone;
    free_cpus_.push(c);
    for (usize i = 0; i < actions_.size();) {
        if (!actions_[i].warp && actions_[i].cpu == c && actions_[i].actor == b) {
            for (usize j = i; j + 1 < actions_.size(); ++j) // in start order, still
                actions_[j] = actions_[j + 1];
            actions_.resize(actions_.size() - 1);
        } else {
            ++i;
        }
    }
    ++tally_.threads_ended;
}

void Soup::death_sweep() {
    // A body dies when it has no running CPU: no un-halted CPU, or no
    // millicore to run on (6.4 #1). Engulfed bodies are already gone.
    // Phase 8: ... and no running warp. A warp whose lanes have all halted
    // has ended, and goes first.
    // Phase 10 (design.md 6.16): with life.threads_end a halted thread leaves
    // its body first; with life.frozen_ticks a body that has run nothing that
    // long dies (an action under way, or a warp's steps, count as running).
    const bool frozen_on = p_->frozen_ticks > 0;
    if (frozen_on) {
        for (usize i = 0; i < actions_.size(); ++i) // waiting on an action isn't frozen
            if (action_live(actions_[i]))
                last_ran_[actions_[i].actor] = tick_;
        if (p_->warps)
            for (u32 w = 0; w < warps_.size(); ++w)
                if (warps_[w].body != kNone && w < wsteps_.size() && wsteps_[w] > 0)
                    last_ran_[warps_[w].body] = tick_;
    }
    for (usize k = 0; k < order_.size(); ++k) {
        u32 b = order_[k];
        Body& B = bodies_[b];
        if (!B.alive)
            continue;
        const u32 sleep = b < sleep_req_.size() ? sleep_req_[b] : 0;
        if (sleep)
            sleep_req_[b] = 0;
        if (p_->dormancy && dorm_[b].on) { // resting: it doesn't run, so it isn't swept as dead
            rest_tick(b);
            continue;
        }
        // docs/inject-plan.md M8, S3: a guest can't sleep, nor a host with
        // guests in it; its ask is dropped.
        if (sleep && (guest_kind_[b] != kGuestNone || has_guests(b))) {
            // (falls through to the ordinary sweep)
        } else if (sleep) { // docs/lifehistory-plan.md V1-V2: a sleep it asked for this tick
            ++tally_.sleeps;
            if (!go_dormant(b, 0, 0, sleep & ~kSleepDrift, (sleep & kSleepDrift) != 0)) {
                ++tally_.sleep_expired; // (no reserve to sleep on: it dies)
                kill(b);
            }
            continue;
        }
        if (p_->threads_end)
            for (u32 c = B.cpus; c != kNone;) {
                const u32 next = cpus_[c].next;
                if (cpus_[c].halted)
                    end_thread(b, c);
                c = next;
            }
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
        if (running && frozen_on && tick_ >= last_ran_[b] + p_->frozen_ticks) {
            if (!(p_->dormancy_auto && go_dormant(b))) { // dormancy: it goes dormant instead, if it can
                ++tally_.died_frozen;
                kill(b);
            }
        } else if (!running) {
            if (B.units == 0 && !(p_->warps && warp_count(b) > 0))
                ++tally_.died_worn;
            else
                ++tally_.died_halted;
            kill(b);
        }
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
    if (disk_on()) // Phase 4, likewise (W4: the archive is in the same ledger)
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
        h.word(0); // (the starvation meter's old place: worlds that never starved hash as before)
    }
    if (p_->warps) { // Phase 8: warps and their lanes (the world fetches any the GPU holds first)
        NOVA_ASSERT(!lanes_on_device());
        h.bytes(warps_.data(), warps_.size() * sizeof(Warp));
        h.bytes(free_warps_.data(), free_warps_.size() * sizeof(u32));
        h.bytes(lanes_.data(), lanes_.size() * sizeof(Lane));
        h.bytes(warp_head_.data(), warp_head_.size() * sizeof(u32));
        h.bytes(warp_count_.data(), warp_count_.size() * sizeof(u32));
        h.bytes(warp_carry_.data(), warp_carry_.size() * sizeof(float));
        if (p_->cost.timed) { // Phase 9b: when each warp may run again; 9c: its clock's carry
            h.bytes(wuntil_.data(), wuntil_.size() * sizeof(u64));
            h.bytes(wtime_.data(), wtime_.size() * sizeof(u32));
        }
    }
    if (p_->cost.timed) { // Phase 9: actions in progress (classic: exactly as before)
        h.word(actions_.size());
        h.bytes(actions_.data(), actions_.size() * sizeof(Action));
    }
    if (kinds_on()) // Phase 9c: the kind each thread prefers
        h.bytes(cpu_kind_.data(), cpu_kind_.size());
    if (ek()) { // Phase 9c: the free E millicores (who holds which is in the unit lists)
        h.word(epool_.size());
        h.bytes(epool_.data(), epool_.size() * sizeof(u32));
    }
    if (p_->frozen_ticks > 0) // Phase 10: when each body last ran
        h.bytes(last_ran_.data(), last_ran_.size() * sizeof(u64));
    if (p_->dormancy) // dormancy: each body slot's dormant state (the tags are detector data)
        h.bytes(dorm_.data(), dorm_.size() * sizeof(Dormant));
    bool guests = false; // docs/inject-plan.md: guests and stomachs (only once there are any: others hash as before)
    for (usize i = 0; i < guest_host_.size() && !guests; ++i)
        guests = guest_host_[i] != kNone || stomach_[i] != kNone;
    if (guests) {
        h.bytes(guest_host_.data(), guest_host_.size() * sizeof(u32));
        h.bytes(guest_kind_.data(), guest_kind_.size());
        h.bytes(guest_at_.data(), guest_at_.size() * sizeof(u32));
        h.bytes(guest_len_.data(), guest_len_.size() * sizeof(u32));
        h.bytes(stomach_.data(), stomach_.size() * sizeof(u32));
    }
    if (!vent_at_.empty()) // Phase 10: the vents' places
        h.bytes(vent_at_.data(), vent_at_.size() * sizeof(u32));
    if (pages_pending()) { // heat inputs carried into the next tick (only then: worlds without them hash as before)
        h.bytes(page_fetch_.data(), page_fetch_.size() * sizeof(u32));
        h.bytes(page_write_.data(), page_write_.size() * sizeof(u32));
    }
    if (p_->disasters) { // disasters X4: the cores offline for repair
        h.word(offline_.size());
        h.bytes(offline_.data(), offline_.size() * sizeof(OfflineCore));
    }
    if (p_->wipe_warning > 0) { // docs/lifehistory-plan.md W5: the wipes to come
        h.word(pending_wipes_.size());
        h.bytes(pending_wipes_.data(), pending_wipes_.size() * sizeof(PendingWipe));
    }
}

bool Soup::pages_pending() const {
    for (usize i = 0; i < page_fetch_.size(); ++i)
        if (page_fetch_[i] | page_write_[i])
            return true;
    return false;
}

// Debugging (nova run/resume --dump-node): this soup's state a section a
// file, <prefix>.<section>, to compare two runs byte by byte, plus the parts
// a snapshot leaves out but a tick carries on with.
void Soup::debug_dump(const char* prefix) const {
    auto out = [&](const char* name, const void* p, usize n) {
        TextBuf path;
        path.append(prefix);
        path.append_char('.');
        path.append(name);
        write_file(path.c_str(), static_cast<const u8*>(p), n);
    };
    const u64 head[2] = {waste_, next_serial_};
    out("head", head, sizeof head);
    out("mem", mem_.data(), mem_.size());
    out("owner", owner_.data(), owner_.size() * sizeof(u32));
    out("blocks", blocks_.data(), blocks_.size() * sizeof(Block));
    out("free_blocks", free_blocks_.data(), free_blocks_.size() * sizeof(u32));
    out("bodies", bodies_.data(), bodies_.size() * sizeof(Body));
    out("free_bodies", free_bodies_.data(), free_bodies_.size() * sizeof(u32));
    out("cpus", cpus_.data(), cpus_.size() * sizeof(Cpu));
    out("free_cpus", free_cpus_.data(), free_cpus_.size() * sizeof(u32));
    out("order", order_.data(), order_.size() * sizeof(u32));
    out("unit_owner", unit_owner_.data(), unit_owner_.size() * sizeof(u32));
    out("unit_heat", unit_heat_.data(), unit_heat_.size() * sizeof(float));
    out("unit_next", unit_next_.data(), unit_next_.size() * sizeof(u32));
    out("unit_prev", unit_prev_.data(), unit_prev_.size() * sizeof(u32));
    out("pool", pool_.data(), pool_.size() * sizeof(u32));
    out("page_heat", page_heat_.data(), page_heat_.size() * sizeof(float));
    out("stamp", stamp_.data(), stamp_.size() * sizeof(u32));
    out("cpu_gen", cpu_gen_.data(), cpu_gen_.size() * sizeof(u32));
    out("cpu_result", cpu_result_.data(), cpu_result_.size() * sizeof(u32));
    out("cpu_pending", cpu_pending_.data(), cpu_pending_.size() * sizeof(u32));
    out("warps", warps_.data(), warps_.size() * sizeof(Warp));
    out("free_warps", free_warps_.data(), free_warps_.size() * sizeof(u32));
    out("lanes", lanes_.data(), lanes_.size() * sizeof(Lane));
    out("warp_head", warp_head_.data(), warp_head_.size() * sizeof(u32));
    out("warp_count", warp_count_.data(), warp_count_.size() * sizeof(u32));
    out("warp_carry", warp_carry_.data(), warp_carry_.size() * sizeof(float));
    out("wuntil", wuntil_.data(), wuntil_.size() * sizeof(u64));
    out("wtime", wtime_.data(), wtime_.size() * sizeof(u32));
    out("actions", actions_.data(), actions_.size() * sizeof(Action));
    out("cpu_kind", cpu_kind_.data(), cpu_kind_.size());
    out("epool", epool_.data(), epool_.size() * sizeof(u32));
    out("last_ran", last_ran_.data(), last_ran_.size() * sizeof(u64));
    out("vent_at", vent_at_.data(), vent_at_.size() * sizeof(u32));
    // Not in a snapshot:
    out("x_wleft", wleft_.data(), wleft_.size() * sizeof(u32));
    out("x_wlive", wlive_.data(), wlive_.size() * sizeof(u32));
    out("x_wact", wact_.data(), wact_.size() * sizeof(u32));
    out("x_wagain", wagain_.data(), wagain_.size() * sizeof(wagain_[0]));
    out("x_eunits", eunits_.data(), eunits_.size() * sizeof(u32));
    out("x_espeed", espeed_.data(), espeed_.size() * sizeof(float));
    out("x_ework", ework_.data(), ework_.size() * sizeof(u32));
    out("x_warp_upkeep", warp_upkeep_.data(), warp_upkeep_.size() * sizeof(double));
    // Later state (Phase 4 onward), for bisecting a snapshot divergence: the
    // disk and its tiers, the unit ledgers, dormancy, the disasters' offline
    // cores; and more of what a snapshot leaves out (x_: derived or working).
    out("disk_mem", disk_.mem.data(), disk_.mem.size());
    out("disk_owner", disk_.owner.data(), disk_.owner.size() * sizeof(u32));
    out("disk_ext", disk_.ext.data(), disk_.ext.size() * sizeof(DiskExtent));
    out("disk_free_ext", disk_.free_ext.data(), disk_.free_ext.size() * sizeof(u32));
    out("disk_total", disk_.total.data(), disk_.total.size() * sizeof(u32));
    out("disk_stamp", disk_.stamp.data(), disk_.stamp.size() * sizeof(u32));
    const u32 dcount[3] = {disk_.free, disk_.afree, disk_.fast};
    out("x_disk_counts", dcount, sizeof dcount);
    out("x_disk_used", disk_.used.data(), disk_.used.size() * sizeof(u64));
    const UnitLedger* led[2] = {&gpu_, &npu_};
    const char* lname[2][5] = {{"gpu_owner", "gpu_pool", "gpu_heat", "gpu_count", "x_gpu_pool_pos"},
                               {"npu_owner", "npu_pool", "npu_heat", "npu_count", "x_npu_pool_pos"}};
    for (u32 i = 0; i < 2; ++i) {
        out(lname[i][0], led[i]->owner.data(), led[i]->owner.size() * sizeof(u32));
        out(lname[i][1], led[i]->pool.data(), led[i]->pool.size() * sizeof(u32));
        out(lname[i][2], led[i]->heat.data(), led[i]->heat.size() * sizeof(float));
        out(lname[i][3], led[i]->count.data(), led[i]->count.size() * sizeof(u32));
        out(lname[i][4], led[i]->pool_pos.data(), led[i]->pool_pos.size() * sizeof(u32));
    }
    out("dorm", dorm_.data(), dorm_.size() * sizeof(Dormant));
    out("offline", offline_.data(), offline_.size() * sizeof(OfflineCore));
    out("pending_wipes", pending_wipes_.data(), pending_wipes_.size() * sizeof(PendingWipe));
    out("page_fetch", page_fetch_.data(), page_fetch_.size() * sizeof(u32));
    out("page_write", page_write_.data(), page_write_.size() * sizeof(u32));
    out("x_used", used_.data(), used_.size() * sizeof(u64));
    out("x_pool_pos", pool_pos_.data(), pool_pos_.size() * sizeof(u32));
    out("x_origin", origin_.data(), origin_.size());
    out("x_wstate", wstate_.data(), wstate_.size());
    out("x_wsteps", wsteps_.data(), wsteps_.size() * sizeof(u32));
    out("x_wbody_chain", wbody_chain_.data(), wbody_chain_.size() * sizeof(u32));
    out("x_emit", emit_.data(), emit_.size() * sizeof(i64));
    out("x_field", field_.data(), field_.size() * sizeof(i32));
    out("x_cpu_time", cpu_time_.data(), cpu_time_.size() * sizeof(u32));
    const u64 misc[10] = {free_epoch_, fail_epoch_, fail_len_, wchain_, wround_, writes_, arrived_dry_, arrived_all_,
                          static_cast<u64>(faults_on_), static_cast<u64>(collapse_)};
    out("x_misc", misc, sizeof misc);
    const double misc_d[5] = {flaw_left_, copy_left_, hazard_mult_, fault_left_, static_cast<double>(ambient_)};
    out("x_misc_d", misc_d, sizeof misc_d);
    out("x_weather", &weather_, sizeof weather_);
    out("x_fever", fever_unit_.data(), fever_unit_.size() * sizeof(u32));
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
    // W4: the archive's free codons follow from its owners (its boundary,
    // `fast`, from the config, as at init).
    afree = 0;
    for (u32 a = fast; a < n; ++a)
        afree += owner[a] == kNone ? 1u : 0u;
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

bool UnitLedger::check(const Array<Body>& bodies, const char* what, TextBuf* why, u32 offline) const {
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
    if (held + pool.size() + offline != n) // (offline: burnt out, disasters X4)
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
    if (disk_on()) // (W4: the archive is in the same ledger, after the fast disk)
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
        w.put_u64(0); // (the starvation meter's old place, kept so the section's shape is unchanged)
    }
    if (p_->warps) {
        NOVA_ASSERT(!lanes_on_device());
        put_array(w, warps_);
        put_array(w, free_warps_);
        put_array(w, lanes_);
        put_array(w, warp_head_);
        put_array(w, warp_count_);
        put_array(w, warp_carry_);
        if (p_->cost.timed) { // Phase 9b, 9c
            put_array(w, wuntil_);
            put_array(w, wtime_);
        }
    }
    if (p_->cost.timed) // Phase 9
        put_array(w, actions_);
    if (kinds_on()) // Phase 9c
        put_array(w, cpu_kind_);
    if (ek())
        put_array(w, epool_);
    if (p_->frozen_ticks > 0) // Phase 10
        put_array(w, last_ran_);
    if (p_->dormancy) // dormancy (v3)
        put_array(w, dorm_);
    put_array(w, guest_host_); // docs/inject-plan.md (v9): guests
    put_array(w, guest_kind_);
    put_array(w, guest_at_);
    put_array(w, guest_len_);
    put_array(w, stomach_);
    if (p_->vent_spacing > 0)
        put_array(w, vent_at_);
    // The pages' heat inputs carried into the next tick (async accelerator
    // results land at the barrier, after the tick's heat update). Snapshots
    // before this (Phase 10 profiling) lost them, so a resumed run drifted.
    if (p_->heat_on) {
        put_array(w, page_fetch_);
        put_array(w, page_write_);
    }
    if (p_->disasters) // disasters X9 (v4): the cores offline for repair
        put_array(w, offline_);
    if (p_->wipe_warning > 0) // docs/lifehistory-plan.md W5 (v5): the wipes to come
        put_array(w, pending_wipes_);
}

bool Soup::load(ByteReader& r, TextBuf* err, u32 version) {
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
    if (disk_on() && !disk_.load(r, nb, err)) // (W4: both tiers; none before v4, see run.cpp)
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
    // Before v3 a map world kept this section whenever its default starvation
    // rule was on (every map world); its meter is read and dropped.
    if (lifecycle() || (version < 3 && p_->map)) {
        u64 dropped = 0;
        if (!(r.get_u64(&writes_) && r.get_u64(&dropped))) {
            err->append("snapshot: lifecycle section truncated");
            return false;
        }
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
        if (p_->cost.timed && (!get_array(r, &wuntil_, many) || wuntil_.size() != warps_.size() ||
                               !get_array(r, &wtime_, many) || wtime_.size() != warps_.size())) {
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
    cpu_kind_.clear();
    if (kinds_on() && (!get_array(r, &cpu_kind_, many) || cpu_kind_.size() != cpus_.size())) { // Phase 9c
        err->append("snapshot: thread kinds truncated or the wrong shape");
        return false;
    }
    // Phase 9c: E millicores. Which units are E follows from the config, as
    // in init; each body's count from its unit list.
    const u32 me = p_->e_share > 0 ? static_cast<u32>(floor(static_cast<double>(m) * p_->e_share + 0.5)) : 0;
    e_first_ = me > 0 ? static_cast<u32>(m) - me : 0xFFFFFFFFu;
    epool_.clear();
    eunits_.clear();
    espeed_.clear();
    eheat_.clear();
    ework_.clear();
    if (ek()) {
        if (!get_array(r, &epool_, m)) {
            err->append("snapshot: E millicores truncated");
            return false;
        }
        eunits_.resize(nb);
        eunits_.fill(0);
        espeed_.resize(nb);
        espeed_.fill(0.0f);
        eheat_.resize(nb);
        eheat_.fill(0.0f);
        ework_.resize(nb);
        ework_.fill(0);
        for (u32 u = e_first_; u < m; ++u)
            if (unit_owner_[u] != kNone && unit_owner_[u] < nb)
                ++eunits_[unit_owner_[u]];
    }
    // Phase 10: when each body last ran; the vents' places.
    last_ran_.clear();
    if (p_->frozen_ticks > 0 && (!get_array(r, &last_ran_, nb) || last_ran_.size() != nb)) {
        err->append("snapshot: frozen watch truncated or the wrong shape");
        return false;
    }
    dorm_.clear();
    dorm_tag_.clear();
    sleep_req_.clear(); // (V1: within a tick; a snapshot is at the barrier)
    if (p_->dormancy) { // (none before v3: no one dormant)
        if (version >= 3) {
            if (!get_array(r, &dorm_, nb) || dorm_.size() != nb) {
                err->append("snapshot: dormant state truncated or the wrong shape");
                return false;
            }
        } else {
            dorm_.resize(nb);
            for (usize i = 0; i < dorm_.size(); ++i)
                dorm_[i] = Dormant{};
        }
        dorm_tag_.resize(nb);
        for (usize i = 0; i < dorm_tag_.size(); ++i)
            dorm_tag_[i] = CargoTag{};
    }
    origin_.clear(); // W7: not in snapshots, so every body alive now began unknown
    origin_.resize(nb);
    // docs/inject-plan.md (v9): guests and stomachs, one per body slot.
    if (!get_array(r, &guest_host_, nb) || guest_host_.size() != nb || !get_array(r, &guest_kind_, nb) ||
        guest_kind_.size() != nb || !get_array(r, &guest_at_, nb) || guest_at_.size() != nb ||
        !get_array(r, &guest_len_, nb) || guest_len_.size() != nb || !get_array(r, &stomach_, nb) ||
        stomach_.size() != nb) {
        err->append("snapshot: guests truncated or the wrong shape");
        return false;
    }
    for (usize i = 0; i < nb; ++i) {
        if (guest_kind_[i] > kGuestSwallowed || (guest_kind_[i] != kGuestNone) != (guest_host_[i] != kNone) ||
            (guest_host_[i] != kNone && guest_host_[i] >= nb) || (stomach_[i] != kNone && stomach_[i] >= blocks_.size())) {
            err->append("snapshot: a guest or stomach out of range");
            return false;
        }
    }
    guests_.clear();
    guests_.resize(nb); // (zeroed; derived)
    for (usize i = 0; i < nb; ++i)
        if (guest_kind_[i] != kGuestNone && guest_host_[i] != kNone)
            ++guests_[guest_host_[i]];
    vent_at_.clear();
    if (p_->vent_spacing > 0 && !get_array(r, &vent_at_, n)) {
        err->append("snapshot: vents truncated");
        return false;
    }
    for (usize i = 0; i < vent_at_.size(); ++i) {
        if (vent_at_[i] >= n) {
            err->append("snapshot: a vent out of range");
            return false;
        }
    }
    page_fetch_.clear();
    page_write_.clear();
    const bool pages = p_->heat_on && version >= 2;
    if (pages && (!get_array(r, &page_fetch_, page_heat_.size()) || page_fetch_.size() != page_heat_.size() ||
                  !get_array(r, &page_write_, page_heat_.size()) || page_write_.size() != page_heat_.size())) {
        err->append("snapshot: page heat inputs truncated or the wrong shape");
        return false;
    }
    if (!pages) {
        page_fetch_.resize(page_heat_.size());
        page_fetch_.fill(0);
        page_write_.resize(page_heat_.size());
        page_write_.fill(0);
    }
    // Disasters X9: the cores offline for repair (v4; none before). check()
    // below demands each is neither held nor free, and counts them.
    offline_.clear();
    weather_ = Weather{};
    clear_fever();
    if (p_->disasters && version >= 4) {
        if (!get_array(r, &offline_, m + p_->gpus)) {
            err->append("snapshot: offline cores truncated");
            return false;
        }
        for (usize i = 0; i < offline_.size(); ++i) {
            const u32 u = offline_[i].unit & ~kOfflineGpu;
            if ((offline_[i].unit & kOfflineGpu) ? u >= p_->gpus : u >= m) {
                err->append("snapshot: an offline core out of range");
                return false;
            }
        }
    }
    // docs/lifehistory-plan.md W5: the wipes to come (v5; none before).
    pending_wipes_.clear();
    if (p_->wipe_warning > 0 && version >= 5) {
        if (!get_array(r, &pending_wipes_, r.remaining() / sizeof(PendingWipe))) {
            err->append("snapshot: pending wipes truncated");
            return false;
        }
        for (usize i = 0; i < pending_wipes_.size(); ++i) {
            const PendingWipe& x = pending_wipes_[i];
            if (x.page >= page_heat_.size() || x.pages == 0 || x.drawn > x.lands || x.false_alarm > 1) {
                err->append("snapshot: a pending wipe out of range");
                return false;
            }
        }
    }
    collapse_ = false; // a snapshot is taken after the barrier applied any collapse
    // The allocator's "no room of this length" cache describes the memory it
    // was noted in. A soup that ran (or was built) before this load could
    // keep one that, with its epoch, would refuse allocations this memory has
    // room for: it starts empty, as it does in a new soup.
    free_epoch_ = 0;
    fail_epoch_ = ~0ull;
    fail_len_ = 0;
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
    for (u32 i = 0; i < epool_.size(); ++i) { // Phase 9c
        if (epool_[i] >= m) {
            err->append("snapshot: E pool out of range");
            return false;
        }
        pool_pos_[epool_[i]] = i;
    }
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
    // Memory tiers (W3): the boundary lies within the soup, on a page (but at share 1).
    if (p_->fast_end > n || (p_->fast_end % kPage && p_->fast_end != n))
        return fail("fast memory's end is off the soup or off a page", p_->fast_end);
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
        if (ek()) { // Phase 9c: its E millicores counted
            u32 e = 0;
            for (u32 u = B.unit_head; u != kNone; u = unit_next_[u])
                e += is_e(u) ? 1 : 0;
            if (b >= eunits_.size() || e != eunits_[b])
                return fail("body E millicore count wrong at body", b);
        }
        units_total += units;
        u32 ncpu = 0;
        for (u32 c = B.cpus; c != kNone; c = cpus_[c].next) {
            if (c >= cpus_.size() || ncpu > cpus_.size())
                return fail("CPU list out of range or cyclic at body", b);
            if (cpus_[c].body != b)
                return fail("CPU list crosses bodies at CPU", c);
            // (a parasite's threads may run in its host's blocks: inject-plan M2)
            if (cpus_[c].block != kNone && blocks_[cpus_[c].block].body != b &&
                !(guest_kind_[b] == kGuestParasite && blocks_[cpus_[c].block].body == guest_host_[b]))
                return fail("CPU wrap block not owned by its body, CPU", c);
            ++ncpu;
        }
        if (ncpu != B.ncpu)
            return fail("body CPU count wrong at body", b);
        // docs/inject-plan.md: a guest's host is a living body that isn't
        // itself; a parasite owns no code of its own.
        if (guest_kind_[b] != kGuestNone) {
            const u32 h = guest_host_[b];
            if (h == kNone || h == b || h >= bodies_.size() || !bodies_[h].alive)
                return fail("guest without a living host, body", b);
        }
    }
    if (!guest_kind_.empty()) { // the guest counts match the guests
        Array<u32> count;
        count.resize(bodies_.size());
        for (usize i = 0; i < bodies_.size(); ++i)
            if (bodies_[i].alive && guest_kind_[i] != kGuestNone && guest_host_[i] < bodies_.size())
                ++count[guest_host_[i]];
        for (usize i = 0; i < bodies_.size(); ++i)
            if (bodies_[i].alive && count[i] != guests_[i])
                return fail("guest count wrong at body", static_cast<u32>(i));
    }
    if (mem_total + waste_ != n)
        return fail("memory not conserved: owned", mem_total);
    // Disasters X4: offline cores are no one's and not free, each listed once.
    u32 off_cpu = 0, off_gpu = 0;
    if (!offline_.empty()) {
        Array<u8> seen;
        seen.resize(static_cast<usize>(p_->millicores) + gpu_.units());
        for (usize i = 0; i < offline_.size(); ++i) {
            const u32 u = offline_[i].unit & ~kOfflineGpu;
            const bool gpu = (offline_[i].unit & kOfflineGpu) != 0;
            if (gpu ? (u >= gpu_.units() || gpu_.owner[u] != kNone || gpu_.pool_pos[u] != kNone)
                    : (u >= p_->millicores || unit_owner_[u] != kNone || pool_pos_[u] != kNone))
                return fail("offline core held, free or out of range at", i);
            u8& s = seen[gpu ? p_->millicores + u : u];
            if (s)
                return fail("offline core listed twice at", i);
            s = 1;
            off_cpu += gpu ? 0 : 1;
            off_gpu += gpu ? 1 : 0;
        }
    }
    if (units_total + pool_.size() + epool_.size() + off_cpu != p_->millicores)
        return fail("CPU units not conserved: owned", units_total);
    for (u32 i = 0; i < pool_.size(); ++i)
        if (unit_owner_[pool_[i]] != kNone || pool_pos_[pool_[i]] != i || is_e(pool_[i]))
            return fail("pool entry inconsistent at", i);
    for (u32 i = 0; i < epool_.size(); ++i) // Phase 9c
        if (unit_owner_[epool_[i]] != kNone || pool_pos_[epool_[i]] != i || !is_e(epool_[i]))
            return fail("E pool entry inconsistent at", i);
    if (ek() && eunits_.size() < bodies_.size())
        return fail("E millicore counts the wrong size", eunits_.size());
    if (p_->frozen_ticks > 0 && last_ran_.size() < bodies_.size()) // Phase 10
        return fail("frozen watch the wrong size", last_ran_.size());
    if (p_->dormancy) { // a resting dormant program holds its code and nothing else
        if (dorm_.size() < bodies_.size() || dorm_tag_.size() < bodies_.size())
            return fail("dormant state the wrong size", dorm_.size());
        for (u32 b = 0; b < bodies_.size(); ++b)
            if (bodies_[b].alive && dorm_[b].on &&
                (bodies_[b].units || bodies_[b].cpus != kNone || (p_->gpus && gpu_.count[b]) ||
                 (p_->npus && npu_.count[b]) || (disk_on() && disk_.total[b]) ||
                 (p_->warps && warp_count_[b])))
                return fail("a dormant body holds more than its code", b);
    }
    if (live != order_.size())
        return fail("schedule order size wrong, live bodies", live);
    for (usize k = 0; k < order_.size(); ++k) {
        if (!bodies_[order_[k]].alive)
            return fail("schedule order holds a dead body at", k);
        if (k > 0 && bodies_[order_[k]].serial <= bodies_[order_[k - 1]].serial)
            return fail("schedule order not by serial at", k);
    }

    // GPU and NPU units: per-body lists, owners, the pool, conservation.
    if (p_->gpus && !gpu_.check(bodies_, "GPU", why, off_gpu))
        return false;
    if (p_->npus && !npu_.check(bodies_, "NPU", why))
        return false;
    if (disk_on() && !disk_.check(bodies_, why))
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
    // Phase 9c: a kind for every CPU slot, P or E.
    if (kinds_on() && cpu_kind_.size() != cpus_.size())
        return fail("thread kinds the wrong size", cpu_kind_.size());
    for (usize i = 0; i < cpu_kind_.size(); ++i)
        if (cpu_kind_[i] > kKindE)
            return fail("thread kind out of range at", i);
    // Phase 9: actions in progress name slots that exist, by live actors (the
    // sweep drops the rest at the end of every tick), one per CPU.
    if (!actions_.empty() && !p_->cost.timed)
        return fail("actions in progress without a timed cost table", actions_.size());
    for (usize i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        if (a.actor >= bodies_.size() || a.cpu >= (a.warp ? warps_.size() : cpus_.size()) || a.warp > 1 ||
            (a.target != kNone && a.target >= bodies_.size()) || a.n > 64 || a.op >= kOpCount ||
            (a.op == static_cast<u32>(Op::engulf) && a.target == kNone && a.at >= p_->size)) // Phase 10
            return fail("action out of range at", i);
        for (usize j = 0; j < i; ++j)
            if (actions_[j].cpu == a.cpu && actions_[j].warp == a.warp && action_live(actions_[j]) &&
                action_live(a))
                return fail("two actions on one CPU or warp at", i);
    }
    return true;
}

} // namespace nova
