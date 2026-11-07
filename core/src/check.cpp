#include "nova/check.h"

#include <cstdio>  // snprintf
#include <cstring> // strncmp, strlen

#include "nova/budget.h"
#include "nova/keys.h" // the settings registry
#include "nova/pool.h"
#include "nova/world.h"

namespace nova {

namespace {

SettingsCheck* g_out = nullptr; // the check the value hook fills

// The registry's value hook: each setting read, and the value it gave back.
void on_value(const Key& k, Str v) {
    SettingsCheck& o = *g_out;
    const usize i = static_cast<usize>(&k - &key_at(0));
    if (i >= o.read.size())
        return;
    o.read[i] = 1;
    o.val_off[i] = static_cast<u32>(o.text.size());
    o.val_len[i] = static_cast<u32>(v.n);
    o.text.append(v.p, v.n);
}

void add_problem(SettingsCheck* out, Str key, Str what) {
    SettingProblem p = {};
    snprintf(p.key, sizeof p.key, "%.*s", static_cast<int>(key.n), key.p);
    snprintf(p.what, sizeof p.what, "%.*s", static_cast<int>(what.n), what.p);
    out->problems.push(p);
}

bool has_problem(const SettingsCheck& out, Str key) {
    for (usize i = 0; i < out.problems.size(); ++i)
        if (key.eq(out.problems[i].key))
            return true;
    return false;
}

// "key: what" (resolve_budgets' errors, and Config's after its "config: ").
void split_problem(Str s, Str* key, Str* what) {
    if (s.starts_with("config: "))
        s = s.sub(8, s.n - 8);
    for (usize i = 0; i + 1 < s.n; ++i) {
        if (s.p[i] == ':' && s.p[i + 1] == ' ') {
            *key = s.sub(0, i);
            *what = s.sub(i + 2, s.n - i - 2);
            return;
        }
    }
    *key = Str();
    *what = s;
}

// The config text without the settings left out so far.
void text_without(const Config& c0, const Array<u32>& left_out, TextBuf* out) {
    out->clear();
    for (usize i = 0; i < c0.count(); ++i) {
        bool skip = false;
        for (usize j = 0; j < left_out.size() && !skip; ++j)
            skip = left_out[j] == i;
        if (skip)
            continue;
        out->append(c0.key(i));
        out->append(" = ");
        out->append(c0.value(i));
        out->append_char('\n');
    }
}

bool is_off(Str s) { return s.eq("off") || s.eq("0") || s.n == 0; }

// LandParams field by field (keep in step with landscape.h; unchanged by the
// builds merged since: substrates, disk tiers, disasters and the lineage
// tracker add none): the landscape cache's key.
bool same_land(const LandParams& a, const LandParams& b) {
    return a.nodes == b.nodes && a.aspect == b.aspect && a.fraction == b.fraction && a.scale == b.scale &&
           a.warp == b.warp && a.border == b.border && a.resolution == b.resolution && a.paths_mean == b.paths_mean &&
           a.paths_spread == b.paths_spread && a.paths_isolated == b.paths_isolated && a.paths_hubs == b.paths_hubs &&
           a.paths_max == b.paths_max && a.paths_connected == b.paths_connected && a.paths_long == b.paths_long &&
           a.latency_scale == b.latency_scale && a.bandwidth == b.bandwidth && a.bridge_count == b.bridge_count &&
           a.bridge_isolated == b.bridge_isolated && a.bridge_max_length == b.bridge_max_length &&
           a.bridge_latency_scale == b.bridge_latency_scale && a.bridge_bandwidth == b.bridge_bandwidth &&
           a.open_every == b.open_every && a.open_ticks == b.open_ticks && a.phase == b.phase;
}

// SubstrateParams field by field (landscape.h): the substrates' part of the
// landscape cache's key, since the summary counts them.
bool same_subs(const SubstrateParams& a, const SubstrateParams& b) {
    bool same = a.smooth == b.smooth && a.scale == b.scale && a.floor_mult == b.floor_mult;
    for (u32 l = 0; l < kSubstrateLevels; ++l)
        same = same && a.share[l] == b.share[l];
    return same;
}

// The distinct substrates over a landscape's nodes (each node's eight levels
// together), as nova land counts them: a landscape's substrate fields are
// static, so its nodes take them at their sites (World::derive_node).
u32 distinct_substrates(u64 seed, const SubstrateParams& sp, const Landscape& L) {
    Array<u8> lv;
    substrate_levels(seed, sp, L.x, L.y, -1.0, &lv);
    const usize n = L.x.size();
    Array<u64> subs;
    for (usize i = 0; i < n; ++i) {
        u64 key = 0;
        for (u32 t = 0; t < kSubstrateTypes; ++t)
            key = key << 8 | lv[i * kSubstrateTypes + t];
        subs.push(key);
    }
    sort_u64(subs.data(), subs.size());
    u32 distinct = 0;
    for (usize i = 0; i < subs.size(); ++i)
        distinct += i == 0 || subs[i] != subs[i - 1];
    return distinct;
}

// nova land's summary of a generated landscape.
void summarise(const Landscape& L, const LandParams& p, LandSummary* s) {
    const u32 n = static_cast<u32>(L.mass.size());
    s->built = true;
    s->width = L.width;
    s->raster_w = L.raster_w;
    s->raster_h = L.raster_h;
    usize cells = 0;
    for (usize c = 0; c < L.land.size(); ++c)
        cells += L.land[c];
    s->land_share = L.land.size() ? static_cast<double>(cells) / static_cast<double>(L.land.size()) : 0;
    s->spacing = L.spacing;
    s->nodes = n;
    s->masses = L.masses;
    Array<u32> per;
    per.resize(L.masses);
    for (u32 i = 0; i < n; ++i)
        if (L.mass[i] < L.masses)
            ++per[L.mass[i]];
    Array<u64> sizes;
    for (u32 m = 0; m < L.masses; ++m)
        if (per[m])
            sizes.push((static_cast<u64>(per[m]) << 32) | m);
    sort_u64(sizes.data(), sizes.size());
    s->peopled = static_cast<u32>(sizes.size());
    for (u32 k = 0; k < 8; ++k)
        s->largest[k] = k < sizes.size() ? static_cast<u32>(sizes[sizes.size() - 1 - k] >> 32) : 0;
    // Land paths per node: keys hold both directions, so count by source.
    Array<u32> deg;
    deg.resize(n);
    u64 land_paths = 0;
    for (usize e = 0; e < L.keys.size(); ++e) {
        const u32 src = static_cast<u32>(L.keys[e] >> 32);
        if (src < n && !L.bridge[e]) {
            ++deg[src];
            ++land_paths;
        }
    }
    for (u32 d = 0; d < 14; ++d)
        s->hist[d] = 0;
    for (u32 i = 0; i < n; ++i)
        ++s->hist[deg[i] < 13 ? deg[i] : 13];
    s->paths_mean = n ? static_cast<double>(land_paths) / n : 0;
    s->hermits = L.hermits;
    s->hubs = L.hubs;
    s->long_links = L.long_added;
    s->bridges = L.bridges;
    s->open_every = p.open_every;
    s->open_ticks = p.open_ticks;
}

} // namespace

void check_settings(Str config_text, SettingsCheck* out, FrontEndReader front_end, bool machine) {
    out->problems.clear();
    out->text.clear();
    out->notes.clear();
    const usize nk = key_count();
    out->read.resize(nk);
    out->read.fill(0);
    out->val_off.resize(nk);
    out->val_len.resize(nk);
    out->soup = out->disk_in_file = out->landscape = false;
    out->nodes = out->soup_size = out->disk = out->archive = 0;
    out->ram = out->ram_avail = out->disk_file = out->disk_free = 0;

    Config c0;
    Config::ParseError pe;
    if (!c0.parse(config_text, &pe)) {
        add_problem(out, Str(), pe.msg);
        return;
    }
    // A key the registry doesn't know: nova run would refuse it (a typo).
    for (usize i = 0; i < c0.count(); ++i) {
        if (key_find(c0.key(i)))
            continue;
        TextBuf t;
        t.append("'");
        t.append(c0.key(i));
        t.append("' is not a setting (a typo?): nova run refuses a key nothing reads");
        add_problem(out, Str(), t.view());
    }

    // --- the reads, as a run makes them ------------------------------------------
    // The budgets' own settings (resolve_budgets reads them; it runs below,
    // on a copy), then the world's (World::init's reads), then the front
    // end's. One problem per pass: its setting is left out (back to its
    // default) and the reads run again.
    Array<u32> left_out;
    bool land = false, subs = false;
    u64 land_seed = 0;
    LandParams lp;
    SubstrateParams sp;
    for (u32 pass = 0; pass < 32; ++pass) {
        TextBuf t;
        text_without(c0, left_out, &t);
        Config c;
        if (!c.parse(t.view(), &pe))
            break;
        if (pass == 0) {
            g_out = out;
            key_set_value_hook(on_value);
        }
        const char* const budgets[] = {"budget.memory",     "budget.disk",     "budget.gpu",     "budget.npu",
                                       "budget.memory_was", "budget.disk_was", "budget.gpu_was", "budget.npu_was"};
        for (const char* b : budgets)
            (void)key_str(c, b);
        World* w = new World;
        w->check_settings(c);
        if (front_end)
            front_end(c);
        if (pass == 0) {
            key_set_value_hook(nullptr);
            g_out = nullptr;
            land = w->landscape();
            land_seed = w->seed();
            lp = w->land_params();
            subs = w->substrates(); // (docs/world-plan.md W2: read before the check returns)
            sp = w->substrate_params();
            out->soup = w->model() == Model::soup;
        }
        delete w;
        if (c.ok())
            break;
        Str key, what;
        split_problem(Str(c.value_error()), &key, &what);
        if (has_problem(*out, key))
            break; // leaving it out didn't help
        add_problem(out, key, what);
        i64 at = -1;
        for (usize i = 0; i < c0.count() && at < 0; ++i)
            if (c0.key(i).eq(key))
                at = static_cast<i64>(i);
        bool already = at < 0;
        for (usize j = 0; j < left_out.size() && !already; ++j)
            already = left_out[j] == static_cast<u32>(at);
        if (already)
            break; // not set (or already left out): nothing more to try
        left_out.push(static_cast<u32>(at));
    }

    // --- the budgets and the fit (budget.h) ---------------------------------------
    {
        Config c;
        if (c.parse(config_text, &pe)) {
            // The device budgets calibrate the real GPU and NPU (a second or
            // so, at the run's start): the check leaves them for then.
            const char* const dev[] = {"budget.gpu", "budget.npu"};
            for (const char* d : dev) {
                const Str v = key_peek_str(c, d);
                if (!is_off(v)) {
                    TextBuf t;
                    t.append(d);
                    t.append(" ");
                    t.append(v);
                    t.append(d[7] == 'g' ? ": a calibration sizes gpu.elements (and warp.capacity) when the run starts\n"
                                         : ": a calibration sizes npu.macs when the run starts\n");
                    out->notes.append(t.view());
                    c.set(d, "off");
                }
            }
            TextBuf notes, err;
            if (machine) {
                if (!resolve_budgets(c, ThreadPool::hardware_threads(), &notes, &err)) {
                    Str key, what;
                    split_problem(err.view(), &key, &what);
                    if (!has_problem(*out, key))
                        add_problem(out, key, what);
                }
                out->notes.append(notes.view());
            }
            // The fit check's own sums (world_footprint, budget.h, shared with
            // resolve_budgets so they can't drift): RAM for the soups at
            // kRamPerCodon a codon, plus the disk tier -- the fast disk and the
            // archive (docs/world-plan.md W4) -- when it lives in RAM; its
            // files otherwise.
            WorldFootprint fp;
            world_footprint(c, &fp);
            TextBuf dpath;
            dpath.append(key_peek_str(c, "disk.path"));
            out->nodes = static_cast<u64>(fp.nodes);
            out->soup_size = static_cast<u64>(fp.soup_size);
            out->disk = static_cast<u64>(fp.disk);
            out->archive = static_cast<u64>(fp.archive);
            out->disk_in_file = fp.in_file;
            if (out->soup) {
                out->ram = fp.ram;
                out->disk_file = fp.disk_file;
            }
            out->ram_avail = static_cast<double>(memory_available());
            u64 free_b = 0;
            if (out->disk_in_file && volume_free(dpath.c_str(), &free_b))
                out->disk_free = static_cast<double>(free_b);
        }
    }

    // --- the landscape (nova land) -----------------------------------------------
    out->landscape = land;
    if (land) {
        if (!out->land_cached || out->land_seed != land_seed || !same_land(out->land_params, lp) ||
            out->land_subs != subs || (subs && !same_subs(out->land_sub_params, sp))) {
            out->land = LandSummary{};
            Landscape* L = new Landscape;
            TextBuf why;
            const double t0 = clock_seconds();
            out->land_why.clear();
            if (generate_landscape(land_seed, lp, L, &why)) {
                summarise(*L, lp, &out->land);
                out->land.substrates = subs; // (nova land's count, docs/world-plan.md W8)
                out->land.substrates_distinct = subs ? distinct_substrates(land_seed, sp, *L) : 0;
                out->land.ms = (clock_seconds() - t0) * 1000.0;
            } else {
                out->land.built = false;
                out->land_why.append(why.view());
            }
            delete L;
            out->land_cached = true;
            out->land_seed = land_seed;
            out->land_params = lp;
            out->land_subs = subs;
            out->land_sub_params = sp;
        }
        if (!out->land.built && !has_problem(*out, "world.nodes"))
            add_problem(out, "world.nodes", out->land_why.view());
    }
}

} // namespace nova
