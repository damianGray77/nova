#include "nova/stats.h"

#include "nova/compress.h"
#include "nova/novelty.h" // the live novelty meter's columns (docs/analysis-plan.md A2)

namespace nova {

namespace {

enum class LinkKind { all, inject, gpu };

u64 total_links(const World& w, LinkKind kind) {
    u64 links = 0;
    for (u32 n = 0; n < w.node_count(); ++n)
        if (const Lineages* L = w.node_lineages(n))
            links += kind == LinkKind::inject ? L->inject_links()
                                              : (kind == LinkKind::gpu ? L->gpu_links() : L->links());
    return links;
}

const char* const kColumns[kStatsColumns] = {
    "tick_first",   "tick_last",    "ticks",         "instructions", "births",        "deaths",
    "sparks",       "halts",        "copy_errors",   "flaws",        "rot_flips",     "cpu_released",
    "mem_released", "engulf_tries", "engulfs",       "inject_tries", "injects",       "upkeep",
    "cpu_worn",     "gpu_elements", "gpu_released",  "gpu_worn",     "npu_macs",      "npu_released",
    "npu_worn",     "sent",         "arrived",       "lost",         "corrupted",     "transit_lost",
    "disk_stores",  "disk_loads",   "disk_compares", "disk_rot",     "wipes",         "wiped",
    "wipe_deaths",  "emits",        "listens",       "collapses",    "recoveries",    "garbled",
    "bodies",       "cpus",         "free_millicores", "waste",      "gpu_held",      "npu_held",
    "in_flight",    "links",        "inject_links",  "gpu_links",    "max_depth",     "lineages",
    "top_lineage",  "genotypes_in_top", "colonised", "links_down",   "disk_held",     "live_nodes",
    // Phase 8: warps (sums, then gauges), after every earlier column so those keep their places
    "warp_steps",   "lane_instr",   "warp_host",     "launches",     "gpu_births",    "warps",
    "gpu_bodies",
    // Phase 9: actions in progress (sums), likewise after every earlier column
    "actions_started", "actions_landed", "actions_failed", "actions_defended",
    // Phase 9c: threads and E millicores (two sums, then a gauge)
    "threads_started", "e_work", "e_held",
    // Phase 10: vents, decay, bites and death (sums)
    "vent_codons",  "decayed",      "bites_waste",   "bites_body",   "codons_bitten", "threads_ended",
    "died_halted",  "died_frozen",  "died_worn",
    // dormancy and drift (five sums, then two gauges)
    "dormant_started", "dormant_woke", "dormant_expired", "dormant_eaten", "dormant_hops", "drifting", "resting",
    // disk tiers (docs/world-plan.md W4): a gauge, then two sums
    "archive_held", "archive_stores", "archive_loads",
    // founders by origin (world plan W7): lineages founded in the window (sums)
    "founded_spark", "founded_daughter", "founded_arrival", "founded_wake",
    // disasters (docs/disasters-plan.md X7): twenty-two sums, then four gauges
    "surges", "surge_nodes", "surge_deaths", "crashes", "crash_deaths", "crash_restarts", "burnouts", "cores_burnt",
    "burnout_dormant", "burnout_graced", "burnout_deaths", "disk_failures", "disk_lost", "storms_formed", "strikes",
    "strike_codons", "episodes", "false_alarms", "spurious_faults", "static_flips", "sounds", "sound_amp",
    "cores_offline", "rebooting", "storms", "episodes_active",
    // memory tiers (docs/world-plan.md W3): a gauge, then two sums
    "fast_held", "fast_allocs", "fast_alloc_fails",
    // behaviour profiles (docs/analysis-plan.md A3): the last whole round's census (gauges)
    "prof_samples", "role_predator", "role_parasite", "role_repairer", "role_signaller", "role_wanderer",
    "role_copier", "role_idle", "work_len", "action_kinds_x100",
    // the live novelty meter (docs/analysis-plan.md A2): two in fixed point (x kNoveltyFixed), then a sum
    "novelty", "novel_share", "novel_success",
    // life history (docs/lifehistory-plan.md W, V7, R7) and cheaper corpses: nine sums
    "wipe_warnings", "wipe_false_alarms", "wipe_flickers", "sleeps", "sleep_woke", "sleep_expired", "ray_inserts",
    "ray_deletes", "corpse_codons",
    // the two floating-point columns, last as always
    "compress",     "ambient"};
// The first 39 tally fields fill columns 3-41; Phase 8's five go after
// live_nodes, then Phase 9's four after the warp gauges, then Phase 9c's two
// and its gauge, then Phase 10's nine, then dormancy's five and its gauges,
// then the disk tiers' gauge and two, then the detector's foundings by origin
// (not tallies: the detector's own counts), then the disasters' 22 tallies
// and their four gauges, then the memory tiers' gauge and two.
inline constexpr u32 kTallyEarly = 39, kTallyWarp = 5, kTallyAction = 4, kTallyThread = 2, kTallyLife = 9,
                     kTallyDormant = 5, kTallyArchive = 2, kFoundedColumns = 4, kTallyDisaster = kStatsDisasterSums,
                     kTallyTier = kStatsTierColumns - 1, kTallyLifeHistory = kStatsLifeColumns;
// The origins of those columns, in order (soup.h).
const u8 kFoundedOrigin[kFoundedColumns] = {kOriginSpark, kOriginDaughter, kOriginArrival, kOriginWake};

u64 total_founded(const World& w, u8 origin) {
    u64 n = 0;
    for (u32 i = 0; i < w.node_count(); ++i)
        if (const Lineages* L = w.node_lineages(i))
            n += L->founded_by(origin);
    return n;
}
static_assert(sizeof(SoupTally) / sizeof(u64) == kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife +
                                                     kTallyDormant + kTallyArchive + kTallyDisaster + kTallyTier +
                                                     kTallyLifeHistory,
              "a SoupTally field was added: update kColumns");

} // namespace

const char* stats_column(u32 i) { return i < kStatsColumns ? kColumns[i] : ""; }

u32 stats_column_index(Str name) {
    for (u32 i = 0; i < kStatsColumns; ++i)
        if (name.eq(kColumns[i]))
            return i;
    return kStatsColumns;
}

void stats_soup_row(const World& w, const SoupTally& sum, u64 first_tick, u32 ticks, StatsMarks* marks, bool compress,
                    StatsRow* out) {
    u64 bodies = 0, cpus = 0, free_units = 0, waste = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const Soup& s = w.node_soup(n);
        bodies += s.body_count();
        for (usize k = 0; k < s.body_count(); ++k)
            cpus += s.body(s.body_at(k)).ncpu;
        free_units += s.free_units();
        waste += s.waste();
    }
    u64* v = out->v;
    v[0] = first_tick;
    v[1] = first_tick + ticks - 1;
    v[2] = ticks;
    const u64* t = reinterpret_cast<const u64*>(&sum);
    for (usize i = 0; i < kTallyEarly; ++i)
        v[3 + i] = t[i];
    // The detector's view: the largest lineage over all nodes, and its
    // genotype count.
    const u64 links = total_links(w, LinkKind::all), inject = total_links(w, LinkKind::inject),
              gpu = total_links(w, LinkKind::gpu);
    u64 gpu_held = 0, npu_held = 0, max_depth = 0, lineages = 0, top = 0, top_genos = 0, colonised = 0,
        disk_held = 0, warps = 0, gpu_bodies = 0, e_held = 0, archive_held = 0, fast_held = 0;
    double comp = 0, ambient = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const Soup& s = w.node_soup(n);
        if (s.warps_on()) { // Phase 8: warps alive, bodies born on a GPU alive
            warps += s.warps_alive();
            for (usize k = 0; k < s.body_count(); ++k)
                gpu_bodies += s.body(s.body_at(k)).form == kFormGpu;
        }
        if (compress)
            comp += estimate_compressibility(s.mem(), s.size()).ratio;
        ambient += s.ambient();
        disk_held += s.params().disk - s.free_disk();
        archive_held += s.params().archive - s.free_archive(); // disk tiers (W4)
        fast_held += s.fast_held();                            // memory tiers (W3)
        e_held += s.e_held(); // Phase 9c
        gpu_held += s.params().gpus - s.free_gpus();
        for (usize i = 0; i < s.offline().size(); ++i) // (disasters X4: burnt GPU units are no one's)
            gpu_held -= (s.offline()[i].unit & kOfflineGpu) ? 1 : 0;
        npu_held += s.params().npus - s.free_npus();
        const Lineages* L = w.node_lineages(n);
        if (!L)
            continue;
        LineageSurvey sv;
        L->survey(&sv);
        max_depth = max_of<u64>(max_depth, sv.max_depth);
        lineages += sv.lineages;
        colonised += sv.colonists > 0;
        if (sv.top_alive > top) {
            top = sv.top_alive;
            top_genos = sv.top_genotypes;
        }
    }
    const u64 tail[] = {bodies,          cpus,        free_units, waste,     gpu_held,     npu_held,
                        w.in_flight(),   links - marks->links, inject - marks->inject, gpu - marks->gpu,
                        max_depth,       lineages,    top,        top_genos, colonised,    w.links_down(),
                        disk_held,       w.live_nodes()};
    const usize nt = sizeof tail / sizeof tail[0];
    static_assert(3 + kTallyEarly + sizeof tail / sizeof tail[0] + kTallyWarp + 2 + kTallyAction + kTallyThread + 1 +
                          kTallyLife + kTallyDormant + 2 + 1 + kTallyArchive + kFoundedColumns + kStatsDisasterColumns +
                          kStatsTierColumns + kStatsProfileColumns + kStatsNoveltyColumns + kStatsLifeColumns ==
                      kStatsIntColumns,
                  "stats row shape");
    for (usize i = 0; i < nt; ++i)
        v[42 + i] = tail[i];
    for (usize i = 0; i < kTallyWarp; ++i)
        v[42 + nt + i] = t[kTallyEarly + i];
    v[42 + nt + kTallyWarp] = warps;
    v[42 + nt + kTallyWarp + 1] = gpu_bodies;
    for (usize i = 0; i < kTallyAction; ++i) // Phase 9
        v[42 + nt + kTallyWarp + 2 + i] = t[kTallyEarly + kTallyWarp + i];
    for (usize i = 0; i < kTallyThread; ++i) // Phase 9c
        v[42 + nt + kTallyWarp + 2 + kTallyAction + i] = t[kTallyEarly + kTallyWarp + kTallyAction + i];
    v[42 + nt + kTallyWarp + 2 + kTallyAction + kTallyThread] = e_held;
    for (usize i = 0; i < kTallyLife; ++i) // Phase 10
        v[42 + nt + kTallyWarp + 2 + kTallyAction + kTallyThread + 1 + i] =
            t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + i];
    const usize dz = 42 + nt + kTallyWarp + 2 + kTallyAction + kTallyThread + 1 + kTallyLife;
    for (usize i = 0; i < kTallyDormant; ++i) // dormancy and drift
        v[dz + i] = t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife + i];
    u64 resting = 0;
    for (u32 n = 0; n < w.node_count(); ++n)
        resting += w.node_soup(n).resting_count();
    v[dz + kTallyDormant] = w.drifting();
    v[dz + kTallyDormant + 1] = resting;
    // Disk tiers (W4): archive held now, then the archive's DISK stores and loads.
    const usize az = dz + kTallyDormant + 2;
    v[az] = archive_held;
    for (usize i = 0; i < kTallyArchive; ++i)
        v[az + 1 + i] = t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife + kTallyDormant + i];
    const usize fz = az + 1 + kTallyArchive;
    for (usize i = 0; i < kFoundedColumns; ++i) { // W7: foundings by origin in the window
        const u64 f = total_founded(w, kFoundedOrigin[i]);
        v[fz + i] = f - marks->founded[i];
        marks->founded[i] = f;
    }
    // Disasters (X7): their sums, then the gauges.
    const usize xz = fz + kFoundedColumns;
    static_assert(3 + 39 + 18 + 5 + 2 + 4 + 2 + 1 + 9 + 5 + 2 + 1 + 2 + 4 == kStatsDisasterFirst, "disasters' first column");
    for (usize i = 0; i < kTallyDisaster; ++i)
        v[xz + i] =
            t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife + kTallyDormant + kTallyArchive + i];
    v[xz + kTallyDisaster] = w.cores_offline();
    v[xz + kTallyDisaster + 1] = w.rebooting_count();
    v[xz + kTallyDisaster + 2] = w.storm_count();
    v[xz + kTallyDisaster + 3] = w.episode_count();
    // Memory tiers (W3): fast memory held now, then the fast ALLOCs' claims and failures.
    const usize mz = xz + kStatsDisasterColumns;
    static_assert(3 + 39 + 18 + 5 + 2 + 4 + 2 + 1 + 9 + 5 + 2 + 1 + 2 + 4 + 26 == kStatsTierFirst, "memory tiers' first column");
    v[mz] = fast_held;
    for (usize i = 0; i < kTallyTier; ++i)
        v[mz + 1 + i] = t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife + kTallyDormant +
                          kTallyArchive + kTallyDisaster + i];
    // Behaviour profiles (A3): the last whole round's census, by the fixed rules.
    {
        static_assert(kRoleCount == 7 && kStatsProfileColumns == 3 + kRoleCount, "the profiles' columns");
        const ProfileCensus& pc = w.profile_census();
        u64* pv = v + kStatsProfileFirst;
        pv[0] = pc.samples;
        for (u32 r = 0; r < kRoleCount; ++r) // (kRole... order: the columns' order)
            pv[1 + r] = pc.role[r];
        pv[1 + kRoleCount] = pc.samples ? (pc.work_len + pc.samples / 2) / pc.samples : 0;
        pv[2 + kRoleCount] = pc.samples ? (pc.kinds * 100 + pc.samples / 2) / pc.samples : 0;
    }
    // The live novelty meter's (A2): the run's, not the world's; stats_novelty_row fills them.
    static_assert(3 + 39 + 18 + 5 + 2 + 4 + 2 + 1 + 9 + 5 + 2 + 1 + 2 + 4 + 26 + kStatsTierColumns + kStatsProfileColumns == kStatsNoveltyFirst, "novelty's first");
    for (u32 i = 0; i < kStatsNoveltyColumns; ++i)
        v[kStatsNoveltyFirst + i] = 0;
    // Life history's sums (docs/lifehistory-plan.md), the last tallies.
    for (u32 i = 0; i < kTallyLifeHistory; ++i)
        v[kStatsLifeFirst + i] = t[kTallyEarly + kTallyWarp + kTallyAction + kTallyThread + kTallyLife + kTallyDormant +
                                   kTallyArchive + kTallyDisaster + kTallyTier + i];
    marks->links = links;
    marks->inject = inject;
    marks->gpu = gpu;
    out->compress = comp / w.node_count();
    out->ambient = ambient / w.node_count();
}

// The live novelty meter's columns (A2): novelty and novel_share in fixed
// point (x kNoveltyFixed, rounded), novel_success a count.
void stats_novelty_row(LiveNovelty* meter, u32 tap, StatsRow* out) {
    if (!meter)
        return;
    const NoveltyTake t = meter->take(tap);
    auto fixed = [](double x) { return static_cast<u64>(x * kNoveltyFixed + 0.5); };
    out->v[kStatsNoveltyFirst] = fixed(t.innovation());
    out->v[kStatsNoveltyFirst + 1] = fixed(t.novel_share());
    out->v[kStatsNoveltyFirst + 2] = t.novel_success;
}

void StatsCsv::set_novelty(LiveNovelty* meter) {
    novelty_ = meter;
    if (meter)
        meter->open_tap(LiveNovelty::kTapStats);
}

void csv_field(TextBuf& out, Str s) {
    bool quote = false;
    for (usize i = 0; i < s.n; ++i) {
        char c = s.p[i];
        if (c == ',' || c == '"' || c == '\r' || c == '\n')
            quote = true;
    }
    if (!quote) {
        out.append(s);
        return;
    }
    out.append_char('"');
    for (usize i = 0; i < s.n; ++i) {
        if (s.p[i] == '"')
            out.append_char('"');
        out.append_char(s.p[i]);
    }
    out.append_char('"');
}

bool StatsCsv::open(const char* path, u32 window, Model model) {
    if (window == 0 || !file_.open(path))
        return false;
    window_ = window;
    ticks_ = 0;
    sum_ = {};
    soup_sum_ = {};
    model_ = model;
    if (model == Model::soup) {
        TextBuf h;
        for (u32 i = 0; i < kStatsColumns; ++i) {
            if (i)
                h.append_char(',');
            h.append(kColumns[i]);
        }
        h.append_char('\n');
        file_.write(h.view());
        marks_ = StatsMarks{};
        return true;
    }
    file_.write("tick_first,tick_last,ticks,parcels_sent,parcels_refused,parcels_delivered,"
                "units_accepted,units_bounced,mutations,units_total,nodes_full,nodes_empty\n");
    return true;
}

bool StatsCsv::open_nodes(const char* path, u32 every) {
    if (!is_open() || model_ != Model::soup || every == 0 || !nodes_.open(path))
        return false;
    nodes_every_ = every;
    node_windows_ = 0;
    nodes_.write("tick_first,tick_last,node,x,y,degree,soup_size,millicores,gpus,npus,cool,spark_rate,"
                 "radiation,bodies,waste,free_millicores,free_gpus,free_npus,births,deaths,sent,arrived,"
                 "lost,lineages,colonists,live,health,life,born,"
                 // a varied world (docs/world-plan.md W8), after every earlier column
                 "sub_p_cores,sub_e_cores,sub_gpu,sub_npu,sub_mem_fast,sub_mem_slow,sub_disk_fast,sub_disk_slow,"
                 "occupancy,"
                 // the disasters' (docs/disasters-plan.md X7, X10)
                 "symptom_ticks,struck,cores_offline\n");
    node_sum_.clear();
    return true;
}

bool StatsCsv::open_signals(const char* path, u32 channels) {
    if (!is_open() || model_ != Model::soup || channels == 0 || !signals_.open(path))
        return false;
    signals_.write("tick_first,tick_last,channel,emits,amplitude,mean_field,sound_amp\n"); // (sound_amp: disasters, X11)
    channels_ = channels;
    sig_emits_.resize(channels);
    sig_emits_.fill(0);
    sig_amp_.resize(channels);
    sig_amp_.fill(0);
    sig_field_.resize(channels);
    sig_field_.fill(0);
    sig_sound_.resize(channels);
    sig_sound_.fill(0);
    return true;
}

void StatsCsv::record(const World& w) {
    if (!is_open())
        return;
    if (ticks_ == 0)
        first_tick_ = w.tick() - 1; // record() runs after the tick completed
    if (model_ == Model::soup) {
        u64* sum = reinterpret_cast<u64*>(&soup_sum_);
        const u64* t = reinterpret_cast<const u64*>(&w.last_soup_tick());
        for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
            sum[i] += t[i];
        if (nodes_.is_open()) {
            node_sum_.resize(static_cast<usize>(w.node_count()) * 7);
            for (u32 n = 0; n < w.node_count(); ++n) {
                const SoupTally& nt = w.node_soup(n).tally();
                u64* s = node_sum_.data() + static_cast<usize>(n) * 7;
                s[0] += nt.births;
                s[1] += nt.deaths;
                s[2] += nt.sent;
                s[3] += nt.arrived;
                s[4] += nt.lost;
                s[5] += w.node_symptoms(n) ? 1 : 0; // disasters (X10)
                s[6] += w.node_struck(n);
            }
        }
        if (signals_.is_open()) {
            for (u32 n = 0; n < w.node_count(); ++n) {
                const Soup& s = w.node_soup(n);
                for (u32 c = 0; c < channels_; ++c) {
                    sig_emits_[c] += s.emit_counts()[c];
                    sig_amp_[c] += s.emit_amps()[c];
                    const i32 f = s.field()[c];
                    sig_field_[c] += (f < 0 ? -static_cast<double>(f) : f) / w.node_count();
                }
            }
            for (u32 c = 0; c < channels_; ++c) // disasters (X11): the world's own sounds
                sig_sound_[c] += w.sound_on_channel(c);
        }
        if (++ticks_ == window_)
            write_row(w, false);
        return;
    }
    const TickStats& t = w.last_tick();
    sum_.parcels_sent += t.parcels_sent;
    sum_.parcels_refused += t.parcels_refused;
    sum_.parcels_delivered += t.parcels_delivered;
    sum_.units_accepted += t.units_accepted;
    sum_.units_bounced += t.units_bounced;
    sum_.mutations += t.mutations;
    if (++ticks_ == window_)
        write_row(w, false);
}

void StatsCsv::finish(const World& w) {
    if (!is_open())
        return;
    if (ticks_ > 0)
        write_row(w, true);
    else if (nodes_.is_open() && node_windows_ > 0)
        write_node_rows(w); // node rows still owed for whole windows
    file_.close();
    nodes_.close();
    signals_.close();
}

void StatsCsv::write_node_rows(const World& w) {
    const SoupParams& mean = w.soup_params();
    TextBuf row;
    auto f6 = [&row](double v) {
        row.append_char(',');
        row.append_f64(static_cast<double>(static_cast<i64>(v * 1e6 + (v < 0 ? -0.5 : 0.5))) / 1e6);
    };
    auto u = [&row](u64 v) {
        row.append_char(',');
        row.append_u64(v);
    };
    // The rows cover every tick since the last node rows (stats.nodes_every).
    const u64 last = node_windows_ ? last_tick_ : first_tick_ + ticks_ - 1;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const Soup& s = w.node_soup(n);
        const SoupParams& p = w.node_params(n);
        row.clear();
        row.append_u64(node_first_);
        u(last);
        u(n);
        f6(w.node_x(n));
        f6(w.node_y(n));
        u(w.degree(n));
        u(p.size);
        u(p.millicores);
        u(p.gpus);
        u(p.npus);
        f6(p.cool);
        f6(p.spark_rate);
        f6(mean.copy_rate > 0 ? p.copy_rate / mean.copy_rate : 1.0);
        u(s.body_count());
        u(s.waste());
        u(s.free_units());
        u(s.free_gpus());
        u(s.free_npus());
        const u64* sum = node_sum_.size() ? node_sum_.data() + static_cast<usize>(n) * 7 : nullptr;
        for (u32 i = 0; i < 5; ++i)
            u(sum ? sum[i] : 0);
        LineageSurvey sv = {};
        if (const Lineages* L = w.node_lineages(n))
            L->survey(&sv);
        u(sv.lineages);
        u(sv.colonists);
        u(w.live(n) ? 1 : 0); // Phase 4 lifecycle
        u(s.lifecycle() && w.live(n) ? s.health() : (w.live(n) ? 100 : 0));
        u(w.life(n));
        u(w.born(n));
        // A varied world (docs/world-plan.md W8): the node's substrate, each
        // type's multiplier (1 without substrates), and its occupancy now.
        for (u32 t = 0; t < kSubstrateTypes; ++t)
            f6(w.substrate_mult(n, t));
        f6(s.occupancy());
        u(sum ? sum[5] : 0); // disasters (X7, X10): symptom ticks, struck, cores offline
        u(sum ? sum[6] : 0);
        u(s.offline_count());
        row.append_char('\n');
        nodes_.write(row.view());
    }
    nodes_.flush();
    node_sum_.fill(0);
    node_windows_ = 0;
}

void StatsCsv::write_row(const World& w, bool final) {
    if (model_ == Model::soup) {
        StatsRow r;
        stats_soup_row(w, soup_sum_, first_tick_, ticks_, &marks_, true, &r);
        stats_novelty_row(novelty_, LiveNovelty::kTapStats, &r); // (A2: the run's meter, this row's window)
        TextBuf row;
        for (u32 i = 0; i < kStatsIntColumns; ++i) {
            if (i)
                row.append_char(',');
            row.append_u64(r.v[i]);
        }
        row.append_char(',');
        row.append_f64(static_cast<double>(static_cast<i64>(r.compress * 1e6 + 0.5)) / 1e6);
        row.append_char(',');
        row.append_f64(static_cast<double>(static_cast<i64>(r.ambient * 1e6 + 0.5)) / 1e6);
        row.append_char('\n');
        file_.write(row.view());
        file_.flush();
        if (nodes_.is_open()) {
            if (node_windows_ == 0)
                node_first_ = first_tick_;
            ++node_windows_;
            last_tick_ = first_tick_ + ticks_ - 1;
            if (node_windows_ >= nodes_every_ || final)
                write_node_rows(w);
        }
        if (signals_.is_open()) {
            TextBuf sig;
            for (u32 c = 0; c < channels_; ++c) {
                sig.clear();
                sig.append_u64(first_tick_);
                sig.append_char(',');
                sig.append_u64(first_tick_ + ticks_ - 1);
                sig.append_char(',');
                sig.append_u64(c);
                sig.append_char(',');
                sig.append_u64(sig_emits_[c]);
                sig.append_char(',');
                sig.append_u64(sig_amp_[c]);
                sig.append_char(',');
                const double mean = sig_field_[c] / ticks_;
                sig.append_f64(static_cast<double>(static_cast<i64>(mean * 1e3 + 0.5)) / 1e3);
                sig.append_char(',');
                sig.append_u64(static_cast<u64>(sig_sound_[c] + 0.5));
                sig.append_char('\n');
                signals_.write(sig.view());
            }
            signals_.flush();
            sig_emits_.fill(0);
            sig_amp_.fill(0);
            sig_field_.fill(0);
            sig_sound_.fill(0);
        }
        ticks_ = 0;
        soup_sum_ = {};
        return;
    }
    u64 full = 0, empty = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        if (w.pool(n) >= w.pool_capacity())
            ++full;
        if (w.pool(n) == 0)
            ++empty;
    }
    const u64 cols[] = {first_tick_,
                        first_tick_ + ticks_ - 1,
                        ticks_,
                        sum_.parcels_sent,
                        sum_.parcels_refused,
                        sum_.parcels_delivered,
                        sum_.units_accepted,
                        sum_.units_bounced,
                        sum_.mutations,
                        w.total_units(),
                        full,
                        empty};
    TextBuf row;
    for (usize i = 0; i < sizeof cols / sizeof cols[0]; ++i) {
        if (i)
            row.append_char(',');
        row.append_u64(cols[i]);
    }
    row.append_char('\n');
    file_.write(row.view());
    file_.flush();
    ticks_ = 0;
    sum_ = {};
}

} // namespace nova
