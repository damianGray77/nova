#include "nova/view.h"

#include <cmath>   // cos, sin, floor, sqrt
#include <cstring> // memcpy, memset

#include "nova/genebank.h"
#include "nova/hash.h"

namespace nova {

namespace {

u32 rgb(double r, double g, double b) {
    auto c = [](double v) -> u32 {
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        return static_cast<u32>(v * 255.0 + 0.5);
    };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

u32 hsv(double h, double s, double v) {
    h = h - floor(h);
    const double x = h * 6.0;
    const int i = static_cast<int>(x) % 6;
    const double f = x - floor(x);
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
    case 0:
        return rgb(v, t, p);
    case 1:
        return rgb(q, v, p);
    case 2:
        return rgb(p, v, t);
    case 3:
        return rgb(p, q, v);
    case 4:
        return rgb(t, p, v);
    default:
        return rgb(v, p, q);
    }
}

// Piecewise-linear ramp through `n` stops (0x00RRGGBB), evenly spaced.
u32 stops(const u32* s, u32 n, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const double x = t * (n - 1);
    u32 i = static_cast<u32>(x);
    if (i >= n - 1)
        return s[n - 1];
    return mix_color(s[i], s[i + 1], x - i);
}

void copy_name(char* dst, Str s) {
    const usize n = s.n < 15 ? s.n : 15;
    memcpy(dst, s.p, n);
    dst[n] = '\0';
}

// Splits CSV text into lines; returns the next line's start.
usize next_line(Str t, usize at, Str* line) {
    usize e = at;
    while (e < t.n && t.p[e] != '\n')
        ++e;
    usize end = e;
    if (end > at && t.p[end - 1] == '\r')
        --end;
    *line = Str(t.p + at, end - at);
    return e < t.n ? e + 1 : t.n;
}

// The fields of a CSV line (numeric files: no quoting needed).
usize split_fields(Str line, Str* out, usize max) {
    usize n = 0, start = 0;
    for (usize i = 0; i <= line.n && n < max; ++i) {
        if (i == line.n || line.p[i] == ',') {
            out[n++] = Str(line.p + start, i - start);
            start = i + 1;
        }
    }
    return n;
}

double field_f64(const Str* f, usize nf, u32 idx) {
    if (idx >= nf)
        return 0;
    double v = 0;
    return parse_f64(f[idx], &v) ? v : 0;
}

// The nodes.csv columns the archive reads, by name.
const char* const kNodeCols[] = {"tick_first", "tick_last", "node",      "x",        "y",         "degree",
                                 "soup_size",  "millicores", "gpus",     "npus",     "cool",      "spark_rate",
                                 "radiation",  "bodies",     "waste",    "free_millicores", "births", "deaths",
                                 "sent",       "arrived",    "lineages", "colonists", "live",     "health",
                                 "life"};
enum NodeCol : u32 {
    kNcFirst, kNcLast, kNcNode, kNcX, kNcY, kNcDegree, kNcSize, kNcMilli, kNcGpus, kNcNpus, kNcCool, kNcSpark,
    kNcRad, kNcBodies, kNcWaste, kNcFree, kNcBirths, kNcDeaths, kNcSent, kNcArrived, kNcLineages, kNcColonists,
    kNcLive, kNcHealth, kNcLife, kNcCount
};
static_assert(sizeof kNodeCols / sizeof kNodeCols[0] == kNcCount, "node column table");

void append_text(Array<char>& dst, Str s) { dst.append(s.p, s.n); }

void genome_text(const CodonTable& t, const u8* codons, u32 len, Array<char>& out) {
    TextBuf tb;
    genome_names(t, codons, len, tb);
    append_text(out, tb.view());
}

void entry_from_record(const GenebankRecord& r, const u8* codons, const CodonTable& t, GenomeEntry* e,
                       Array<char>& text) {
    memset(e, 0, sizeof *e);
    e->tick = r.tick;
    e->genotype = r.genotype;
    e->parent = r.parent;
    e->lineage = r.lineage;
    e->node = r.node;
    e->len = r.len;
    TextBuf nm;
    Genebank::format_name(r.len, r.name, nm);
    copy_name(e->name, nm.view());
    nm.clear();
    if (r.parent_name != Genebank::kNoName)
        Genebank::format_name(r.parent_len, r.parent_name, nm);
    else
        nm.append(r.parent ? "unsaved" : "none");
    copy_name(e->parent_name, nm.view());
    e->off = static_cast<u32>(text.size());
    genome_text(t, codons + r.codon_off, r.len, text);
    e->n = static_cast<u32>(text.size()) - e->off;
}

// A lineage id written "n.k".
bool parse_lineage(Str s, u64* id) {
    const usize dot = s.find('.');
    u64 hi = 0, lo = 0;
    if (dot >= s.n || !parse_u64(s.sub(0, dot), &hi) || !parse_u64(s.sub(dot + 1, s.n - dot - 1), &lo))
        return false;
    *id = (hi << 32) | (lo & 0xFFFFFFFFull);
    return true;
}

// The value of `key=` in a space-separated record line.
Str kv(Str line, const char* key) {
    const Str k(key);
    for (usize i = 0; i + k.n <= line.n; ++i) {
        if ((i == 0 || line.p[i - 1] == ' ') && Str(line.p + i, k.n).eq(k) && i + k.n < line.n &&
            line.p[i + k.n] == '=') {
            usize s = i + k.n + 1, e = s;
            while (e < line.n && line.p[e] != ' ')
                ++e;
            return Str(line.p + s, e - s);
        }
    }
    return Str();
}

} // namespace

// --- colours ---------------------------------------------------------------

u32 mix_color(u32 a, u32 b, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    auto ch = [&](int sh) {
        const double x = ((a >> sh) & 255) + (static_cast<double>((b >> sh) & 255) - ((a >> sh) & 255)) * t;
        return static_cast<u32>(x + 0.5) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

u32 lineage_color(u64 id) {
    if (id == 0)
        return 0x808080;
    const u64 h = mix64(id ^ 0x6C696E6561676521ull);
    const double hue = static_cast<double>(h & 0xFFFF) / 65536.0;
    const double sat = 0.55 + 0.35 * static_cast<double>((h >> 16) & 0xFF) / 255.0;
    const double val = 0.80 + 0.20 * static_cast<double>((h >> 24) & 0xFF) / 255.0;
    return hsv(hue, sat, val);
}

u32 ramp_color(double t) {
    static const u32 s[] = {0x440154, 0x3B528B, 0x21918C, 0x5EC962, 0xFDE725};
    return stops(s, 5, t);
}

u32 heat_color(double t) {
    static const u32 s[] = {0x101010, 0x800000, 0xFF4000, 0xFFB400, 0xFFFFFF};
    return stops(s, 5, t);
}

// --- names -----------------------------------------------------------------

const char* color_by_name(u32 c) {
    static const char* const n[kColorCount] = {"bodies",   "births",     "colonists",  "health",    "lineage",
                                               "free CPU", "waste",      "memory",     "millicores", "climate",
                                               "sparks",   "radiation",  "disk",       "volatility", "signals",
                                               "GPU lanes", "E-cores", "material"};
    return c < kColorCount ? n[c] : "?";
}

const char* strip_by_name(u32 s) {
    static const char* const n[kStripCount] = {"owner", "lineage", "page heat", "salvage stamp", "codon"};
    return s < kStripCount ? n[s] : "?";
}

const char* event_kind_name(u32 k) {
    static const char* const n[kEvCount] = {"replicator", "gpu_replicator", "npu_user",   "colonist",
                                            "disk_user",  "signaller",      "gpu_native", "defended",
                                            "collapse",   "recovered",      "event"};
    return k < kEvCount ? n[k] : "?";
}

void parse_event(Str text, u32* kind, u32* node) {
    *kind = kEvOther;
    for (u32 k = 0; k < kEvOther; ++k) {
        TextBuf p;
        p.append(event_kind_name(k));
        p.append(" node=");
        if (text.starts_with(p.view())) {
            *kind = k;
            break;
        }
    }
    *node = kNone;
    const Str v = kv(text, "node");
    u64 n = 0;
    if (v.n && parse_u64(v, &n) && n < kNone)
        *node = static_cast<u32>(n);
}

// --- metrics ---------------------------------------------------------------

double node_metric(const NodeView& n, u32 c) {
    switch (c) {
    case kColorBodies:
        return n.bodies;
    case kColorBirths:
        return n.births;
    case kColorColonists:
        return n.colonists;
    case kColorHealth:
        return n.live ? n.health : 0;
    case kColorLineage:
        return n.top_alive;
    case kColorFreeCpu:
        return n.millicores ? static_cast<double>(n.free_millicores) / n.millicores : 0;
    case kColorWaste:
        return n.soup_size ? static_cast<double>(n.waste) / n.soup_size : 0;
    case kColorMemory:
        return n.soup_size;
    case kColorMillicores:
        return n.millicores;
    case kColorClimate:
        return n.cool;
    case kColorSparks:
        return n.spark_rate;
    case kColorRadiation:
        return n.radiation;
    case kColorDisk:
        return n.disk;
    case kColorVolatility:
        return n.volatility;
    case kColorSignals:
        return n.emits;
    case kColorLanes:
        return n.lanes;
    case kColorEcores:
        return n.millicores ? static_cast<double>(n.emillicores) / n.millicores : 0;
    case kColorMaterial:
        return n.material;
    default:
        return 0;
    }
}

bool metric_range(u32 c, double* lo, double* hi) {
    *lo = 0;
    switch (c) {
    case kColorHealth:
        *hi = 100;
        return true;
    case kColorFreeCpu:
    case kColorWaste:
    case kColorEcores:
    case kColorMaterial:
        *hi = 1;
        return true;
    default:
        *hi = 1;
        return false;
    }
}

// --- frames and charts -----------------------------------------------------

void ViewFrame::clear_new() {
    sample_tick.clear();
    sample.clear();
    events.clear();
    event_text.clear();
    saves.clear();
    save_text.clear();
}

// Sums: the early tallies, the link counts, the warp tallies, Phase 9's
// actions and 9c's thread tallies, Phase 10's (e_held, 73, is a gauge),
// dormancy's five (drifting and resting, 88 and 89, are gauges), and the disk
// tiers' archive stores and loads (91, 92; archive_held, 90, is a gauge).
bool stats_column_is_sum(u32 i) {
    return (i >= 3 && i < 42) || (i >= 49 && i <= 51) || (i >= 60 && i <= 64) || (i >= 67 && i <= 72) ||
           (i >= 74 && i <= 82) || (i >= 83 && i <= 87) || (i >= 91 && i <= 92);
}

void chart_row(const StatsRow& r, double* out) {
    const double ticks = static_cast<double>(r.v[2]);
    for (u32 i = 0; i < kStatsColumns; ++i) {
        const double v = r.value(i);
        out[i] = stats_column_is_sum(i) && ticks > 0 ? v / ticks : v;
    }
}

void ChartLog::clear() {
    ticks_.clear();
    vals_.clear();
    step_ = 1;
    skip_ = 0;
}

void ChartLog::add(u64 tick, const double* row) {
    if (skip_ > 0) {
        --skip_;
        return;
    }
    ticks_.push(tick);
    vals_.append(row, kStatsColumns);
    skip_ = step_ - 1;
    if (ticks_.size() < kChartPoints)
        return;
    // Full: keep every other point (the even ones). The newest, just added,
    // goes; the last kept is one old step before it, so the next point to keep
    // is one old step after it: skip the old step's worth of samples less one.
    usize w = 0;
    for (usize i = 0; i < ticks_.size(); i += 2, ++w) {
        ticks_[w] = ticks_[i];
        memmove(vals_.data() + w * kStatsColumns, vals_.data() + i * kStatsColumns, kStatsColumns * sizeof(double));
    }
    ticks_.resize(w);
    vals_.resize(w * kStatsColumns);
    skip_ = step_ - 1;
    step_ *= 2;
}

// --- the phylogeny ---------------------------------------------------------

void phylo_layout(const GenomeEntry* e, usize n, u64 lineage, Array<PhyloNode>* out) {
    out->clear();
    // The lineage's genotypes, first save only, in save order (= tick order).
    Array<u64> key; // genotype, then its layout index, for lookup by sorting
    for (usize i = 0; i < n; ++i) {
        if (e[i].lineage != lineage)
            continue;
        bool seen = false;
        for (usize j = 0; j < out->size() && !seen; ++j)
            seen = e[(*out)[j].entry].genotype == e[i].genotype;
        if (seen)
            continue;
        PhyloNode p = {};
        p.entry = static_cast<u32>(i);
        p.parent = kNone;
        p.tick = e[i].tick;
        out->push(p);
    }
    const usize m = out->size();
    if (m == 0)
        return;
    for (usize i = 0; i < m; ++i) {
        const u64 parent = e[(*out)[i].entry].parent;
        if (!parent)
            continue;
        for (usize j = 0; j < m; ++j) {
            if (j != i && e[(*out)[j].entry].genotype == parent) {
                (*out)[i].parent = static_cast<u32>(j);
                break;
            }
        }
    }
    // Depth-first, children in save order. A cycle (a genotype forgotten and
    // seen again with a later parent) can't be reached from a true root; its
    // first member becomes a root itself.
    Array<u8> done;
    done.resize(m);
    Array<u32> stack;
    u32 row = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        for (usize r = 0; r < m; ++r) {
            if (done[r] || (pass == 0 && (*out)[r].parent != kNone))
                continue;
            if (pass == 1)
                (*out)[r].parent = kNone; // in a cycle
            stack.clear();
            stack.push(static_cast<u32>(r));
            while (!stack.empty()) {
                const u32 v = stack.back();
                stack.pop();
                if (done[v])
                    continue;
                done[v] = 1;
                PhyloNode& pv = (*out)[v];
                pv.row = row++;
                pv.depth = pv.parent == kNone ? 0 : (*out)[pv.parent].depth + 1;
                // Children pushed in reverse save order, so they pop in order.
                for (usize c = m; c-- > 0;)
                    if (!done[c] && (*out)[c].parent == v)
                        stack.push(static_cast<u32>(c));
            }
        }
    }
}

// --- positions -------------------------------------------------------------

void map_positions(const World& w, Array<float>* x, Array<float>* y) {
    const u32 n = w.node_count();
    x->resize(n);
    y->resize(n);
    const double kPi = 3.14159265358979323846;
    // A landscape is world_width() x 1: scale it into the unit box, centred.
    const double W = w.world_width();
    const double s = W > 1 ? 1.0 / W : 1.0;
    const double ox = (1.0 - W * s) * 0.5, oy = (1.0 - s) * 0.5;
    for (u32 i = 0; i < n; ++i) {
        if (w.landscape()) {
            (*x)[i] = static_cast<float>(ox + w.node_x(i) * s);
            (*y)[i] = static_cast<float>(oy + w.node_y(i) * s);
        } else if (w.procedural()) {
            (*x)[i] = static_cast<float>(w.node_x(i));
            (*y)[i] = static_cast<float>(w.node_y(i));
        } else {
            const double a = 2 * kPi * i / (n ? n : 1) - kPi / 2;
            (*x)[i] = static_cast<float>(0.5 + 0.45 * cos(a));
            (*y)[i] = static_cast<float>(0.5 + 0.45 * sin(a));
        }
    }
}

void land_view(const World& w, ViewFrame* f) {
    f->landscape = w.landscape();
    f->land_sent = false;
    f->land.clear();
    f->masses.clear();
    f->land_w = f->land_h = 0;
    if (!w.landscape())
        return;
    const Landscape& L = w.land();
    const double W = L.width;
    const double s = W > 1 ? 1.0 / W : 1.0;
    const double ox = (1.0 - W * s) * 0.5, oy = (1.0 - s) * 0.5;
    f->box[0] = static_cast<float>(ox);
    f->box[1] = static_cast<float>(oy);
    f->box[2] = static_cast<float>(ox + W * s);
    f->box[3] = static_cast<float>(oy + s);
    f->land_w = L.raster_w;
    f->land_h = L.raster_h;
    f->land.resize(L.land.size());
    for (usize c = 0; c < L.land.size(); ++c) {
        const u32 m = L.cell_mass[c];
        f->land[c] = m == 0xFFFFFFFFu ? 0x0E1A2C : mix_color(0x37442F, lineage_color(m + 1), 0.12);
    }
    f->land_sent = true;
    // Each landmass with nodes, labelled at its nodes' centroid.
    Array<double> sx, sy;
    Array<u32> count;
    sx.resize(L.masses);
    sy.resize(L.masses);
    count.resize(L.masses);
    for (u32 n = 0; n < w.node_count(); ++n) {
        const u32 m = L.mass[n];
        sx[m] += ox + L.x[n] * s;
        sy[m] += oy + L.y[n] * s;
        ++count[m];
    }
    for (u32 m = 0; m < L.masses; ++m) {
        if (!count[m])
            continue;
        MassView v;
        v.x = static_cast<float>(sx[m] / count[m]);
        v.y = static_cast<float>(sy[m] / count[m]);
        v.id = m;
        v.nodes = count[m];
        f->masses.push(v);
    }
}

// --- the live session ------------------------------------------------------

void ViewSession::attach(Run* run, u32 sample_every) {
    run_ = run;
    every_ = sample_every;
    sum_ = {};
    ticks_ = 0;
    first_ = run->world().tick();
    marks_ = StatsMarks{};
    // Links so far are history, not this window's.
    StatsRow scratch;
    if (run->world().model() == Model::soup)
        stats_soup_row(run->world(), sum_, 0, 1, &marks_, false, &scratch);
    acc_.resize(static_cast<usize>(run->world().node_count()) * 5);
    acc_.fill(0);
    acc_ticks_ = 0;
    new_tick_.clear();
    new_rows_.clear();
    events_sent_ = 0;
    px_.clear();
    py_.clear();
    hash_valid_ = false;
    land_sent_ = false;
}

void ViewSession::after_step() {
    const World& w = run_->world();
    if (w.model() != Model::soup)
        return;
    if (ticks_ == 0)
        first_ = w.tick() - 1;
    u64* s = reinterpret_cast<u64*>(&sum_);
    const u64* t = reinterpret_cast<const u64*>(&w.last_soup_tick());
    for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
        s[i] += t[i];
    ++ticks_;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const SoupTally& nt = w.node_soup(n).tally();
        float* a = acc_.data() + static_cast<usize>(n) * 5;
        a[0] += static_cast<float>(nt.births);
        a[1] += static_cast<float>(nt.deaths);
        a[2] += static_cast<float>(nt.sent);
        a[3] += static_cast<float>(nt.arrived);
        a[4] += static_cast<float>(nt.emits);
    }
    ++acc_ticks_;
    if (every_ && ticks_ >= every_)
        sample();
}

void ViewSession::sample() {
    StatsRow r;
    stats_soup_row(run_->world(), sum_, first_, ticks_, &marks_, true, &r);
    double row[kStatsColumns];
    chart_row(r, row);
    new_tick_.push(r.v[1] + 1); // ticks completed at the window's end
    new_rows_.append(row, kStatsColumns);
    sum_ = {};
    ticks_ = 0;
}

void ViewSession::flush_sample() {
    if (ticks_ > 0)
        sample();
}

void ViewSession::build(const ViewRequest& q, ViewFrame* f) {
    if (q.node != kNone && q.node < run_->world().node_count())
        run_->world().warp_sync(q.node); // Phase 8: the inspected node's lanes, back from the GPU
    const World& w = run_->world();
    const u32 nodes = w.node_count();
    f->archive = false;
    f->kinds = false;
    f->tick = w.tick();
    f->instructions = run_->instructions();
    f->nodes = nodes;
    f->procedural = w.spatial();
    f->detect_tick = run_->detection().found ? run_->detection().tick + 1 : 0;
    f->hashed = q.hash;
    f->hash = 0;
    if (q.hash) { // it reads the whole world: once per tick at most
        if (!hash_valid_ || hash_tick_ != w.tick()) {
            hash_ = w.hash();
            hash_tick_ = w.tick();
            hash_valid_ = true;
        }
        f->hash = hash_;
    }
    if (px_.size() != nodes)
        map_positions(w, &px_, &py_);
    // A landscape's land, once.
    if (!land_sent_) {
        land_view(w, f);
        land_sent_ = true;
    } else {
        f->landscape = w.landscape();
        f->land_sent = false;
        f->land.clear();
        f->masses.clear();
    }

    f->node.resize(nodes);
    if (w.model() == Model::soup) {
        const SoupParams& mean = w.soup_params();
        const float inv = acc_ticks_ ? 1.0f / static_cast<float>(acc_ticks_) : 0.0f;
        for (u32 n = 0; n < nodes; ++n) {
            NodeView& v = f->node[n];
            memset(&v, 0, sizeof v);
            const Soup& s = w.node_soup(n);
            const SoupParams& p = w.node_params(n);
            v.x = px_[n];
            v.y = py_[n];
            v.live = w.live(n) ? 1 : 0;
            v.life = w.life(n);
            v.bodies = static_cast<u32>(s.body_count());
            for (usize k = 0; k < s.body_count(); ++k)
                v.cpus += s.body(s.body_at(k)).ncpu;
            v.health = s.lifecycle() && v.live ? s.health() : (v.live ? 100 : 0);
            const Lineages* L = w.node_lineages(n);
            if (L) {
                LineageSurvey sv;
                L->survey(&sv);
                v.lineages = sv.lineages;
                v.colonists = sv.colonists;
                v.top_alive = sv.top_alive;
                v.top_lineage = sv.top_lineage;
                if (q.genotype) {
                    for (usize k = 0; k < s.body_count(); ++k) {
                        const u32 blk = s.body(s.body_at(k)).blocks;
                        if (blk == kNone)
                            continue;
                        const BlockTag tag = L->tag(blk);
                        if (tag.lineage && tag.genotype == q.genotype)
                            ++v.holding;
                    }
                }
            }
            v.soup_size = p.size;
            v.millicores = p.millicores;
            v.emillicores = s.e_total();
            { // Phase 10: the share of its waste that isn't dead matter (every 16th codon)
                u32 waste = 0, live = 0;
                for (u32 a = 0; a < p.size; a += 16) {
                    if (s.owner_block(a) != kNone)
                        continue;
                    ++waste;
                    live += p.table.op[s.mem()[a] & 63] != Op::inert;
                }
                v.material = waste ? static_cast<float>(live) / waste : 0.0f;
            }
            v.free_millicores = s.free_units();
            v.waste = s.waste();
            v.gpus = p.gpus;
            v.npus = p.npus;
            v.disk = p.disk;
            v.degree = w.degree(n);
            const float* a = acc_.data() + static_cast<usize>(n) * 5;
            v.births = a[0] * inv;
            v.deaths = a[1] * inv;
            v.sent = a[2] * inv;
            v.arrived = a[3] * inv;
            v.emits = a[4] * inv;
            v.cool = p.cool;
            v.spark_rate = static_cast<float>(p.spark_rate);
            v.radiation = static_cast<float>(mean.copy_rate > 0 ? p.copy_rate / mean.copy_rate : 1.0);
            v.volatility = static_cast<float>(p.volatility);
            v.known = (1u << kColorCount) - 1;
            v.mass = w.node_mass(n);
            if (s.warps_on()) { // Phase 8: lanes still running, bodies born on a GPU
                for (usize wi = 0; wi < s.warp_slots(); ++wi)
                    if (s.warp(static_cast<u32>(wi)).body != kNone)
                        v.lanes += s.warp_live(static_cast<u32>(wi));
                for (usize k = 0; k < s.body_count(); ++k)
                    v.gpu_bodies += s.body(s.body_at(k)).form == kFormGpu;
            }
        }
        acc_.fill(0);
        acc_ticks_ = 0;
    } else {
        for (u32 n = 0; n < nodes; ++n) {
            NodeView& v = f->node[n];
            memset(&v, 0, sizeof v);
            v.x = px_[n];
            v.y = py_[n];
            v.live = 1;
            v.health = 100;
            v.bodies = w.pool(n); // the demo model's units
            v.known = 1u << kColorBodies;
        }
    }

    // Edges, each undirected link once; long links stand out.
    f->edge.clear();
    const double spacing = 1.0 / sqrt(static_cast<double>(nodes ? nodes : 1));
    for (u32 e = 0; e < w.edge_count(); ++e) {
        const u32 a = w.edge_src(e), b = w.edge_dst(e);
        if (a >= b)
            continue;
        EdgeView ev = {};
        ev.a = a;
        ev.b = b;
        ev.down = w.edge_down(e) ? 1 : 0;
        ev.bridge = w.edge_bridge(e) ? 1 : 0;
        ev.shut = w.edge_shut(e) ? 1 : 0;
        ev.long_link = !ev.bridge && w.spatial() && w.edge_distance(e) > 2.5 * spacing ? 1 : 0;
        f->edge.push(ev);
    }

    // Cargo in flight, interpolated along its edge by send and due tick.
    f->cargo.clear();
    for (usize i = 0; i < w.in_flight(); ++i) {
        const u32 e = w.cargo_edge(i);
        const u32 a = w.edge_src(e), b = w.edge_dst(e);
        const double ticks = w.cargo_ticks(i) ? w.cargo_ticks(i) : 1;
        const double sent = static_cast<double>(w.cargo_due(i)) - ticks;
        double t = (static_cast<double>(w.tick()) - sent) / ticks;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        CargoView c = {};
        c.x = static_cast<float>(px_[a] + (px_[b] - px_[a]) * t);
        c.y = static_cast<float>(py_[a] + (py_[b] - py_[a]) * t);
        c.len = w.cargo_len(i);
        f->cargo.push(c);
    }

    // Knobs.
    f->knobs = min_of(w.knob_count(), kMaxKnobs);
    for (u32 i = 0; i < f->knobs; ++i) {
        copy_name(f->knob_key[i], w.knob_key(i)); // keys are short ("edge.corruption")
        f->knob_value[i] = w.knob_now(i);
    }

    // The selected node: its memory strip and bodies, then the selected body.
    f->sel_node = kNone;
    f->strip.clear();
    f->strip_owner.clear();
    f->strip_w = f->strip_h = 0;
    f->bodies.clear();
    f->sel_body = kNone;
    f->req_body = q.body;
    f->req_serial = q.serial;
    f->body_ok = false;
    f->genome.clear();
    f->cpus.clear();
    f->lanes.clear();
    if (w.model() == Model::soup && q.node < nodes) {
        f->sel_node = q.node;
        const Soup& s = w.node_soup(q.node);
        f->kinds = s.params().kinds; // Phase 9c
        const Lineages* L = w.node_lineages(q.node);
        const u32 size = s.size();
        f->strip_w = 256;
        f->strip_h = (size + 255) / 256;
        f->strip.resize(static_cast<usize>(f->strip_w) * f->strip_h);
        f->strip.fill(0x202020);
        f->strip_owner.resize(f->strip.size());
        f->strip_owner.fill(kNone);
        u32 sel_body = kNone;
        for (usize k = 0; k < s.body_count(); ++k)
            if (s.body_at(k) == q.body && s.body(q.body).serial == q.serial)
                sel_body = q.body;
        const float hmax = s.params().mem.max > 0 ? s.params().mem.max : 1.0f;
        for (u32 a = 0; a < size; ++a) {
            const u32 owner = s.owner_body(a);
            u32 c = 0x303030;
            const bool inert = s.params().table.op[s.mem()[a] & 63] == Op::inert; // Phase 10: dead matter
            switch (q.strip) {
            case kStripOwner:
                c = owner == kNone ? (inert ? 0x181818 : 0x303030) : lineage_color(s.body(owner).serial + 1);
                break;
            case kStripLineage: {
                if (owner == kNone) {
                    c = 0x303030;
                } else {
                    const BlockTag tag = L ? L->tag(s.owner_block(a)) : BlockTag{};
                    c = tag.lineage ? lineage_color(tag.lineage) : 0x707070;
                }
                break;
            }
            case kStripHeat:
                c = heat_color(s.page_heat(a >> kPageBits) / hmax);
                break;
            case kStripStamp: {
                const u32 st = s.stamp(a);
                c = st ? ramp_color(log(static_cast<double>(st)) / log(4096.0)) : 0x303030;
                break;
            }
            default: // codon (INERT: dark)
                c = inert ? 0x0A0A0A : hsv((s.mem()[a] & 63) / 64.0, 0.6, 0.9);
                break;
            }
            if (sel_body != kNone && owner == sel_body)
                c = mix_color(c, 0xFFFFFF, 0.6);
            f->strip[a] = c;
            f->strip_owner[a] = owner;
        }
        // Phase 10: the vents, marked orange (three codons wide, to be seen).
        const Array<u32>& vents = s.vent_places();
        for (usize i = 0; i < vents.size(); ++i)
            for (u32 d = 0; d < 3; ++d) {
                const u32 a = (vents[i] + size - 1 + d) % size;
                f->strip[a] = 0xFF8C1A;
            }
        const usize cap = 4096;
        for (usize k = 0; k < s.body_count() && k < cap; ++k) {
            const u32 b = s.body_at(k);
            const Body& B = s.body(b);
            BodyRow r = {};
            r.serial = B.serial;
            r.slot = b;
            r.ncpu = B.ncpu;
            r.units = B.units;
            r.eunits = s.e_units(b);
            r.mem = B.mem;
            r.gpus = s.gpu_count(b);
            r.npus = s.npu_count(b);
            r.disk = s.disk_held(b);
            r.form = B.form;
            r.warps = s.warp_count(b);
            if (!s.actions().empty()) { // Phase 9: what it is doing, and what is being done to it
                const u64 now = s.current_tick();
                for (usize i = 0; i < s.actions().size(); ++i) {
                    const Action& a = s.actions()[i];
                    if (a.actor == b && a.actor_serial == B.serial && s.action_live(a) && r.doing_op == 0) {
                        r.doing_op = a.op;
                        r.doing_left = a.end > now ? static_cast<u32>(a.end - now) : 0;
                    }
                }
                const u32 on = s.action_on(b);
                if (on != kNone) {
                    const Action& a = s.actions()[on];
                    r.threat_op = a.op;
                    r.threat_left = a.end > now ? static_cast<u32>(a.end - now) : 0;
                }
            }
            if (B.blocks != kNone) {
                r.start = s.block(B.blocks).start;
                r.len = s.block(B.blocks).len;
                if (L) {
                    const BlockTag tag = L->tag(B.blocks);
                    r.lineage = tag.lineage;
                    r.genotype = tag.genotype;
                    r.depth = tag.depth;
                }
            }
            f->bodies.push(r);
            if (b == sel_body) {
                f->body = r;
                f->body_ok = true;
            }
        }
        if (sel_body != kNone && !f->body_ok) { // past the list's cap
            const Body& B = s.body(sel_body);
            f->body = BodyRow{};
            f->body.serial = B.serial;
            f->body.slot = sel_body;
            f->body_ok = true;
        }
        if (f->body_ok) {
            f->sel_body = sel_body;
            const Body& B = s.body(sel_body);
            if (B.blocks != kNone)
                genome_text(s.params().table, s.mem() + s.block(B.blocks).start, s.block(B.blocks).len, f->genome);
            for (u32 c = B.cpus; c != kNone && f->cpus.size() < 64; c = s.cpu(c).next) {
                const Cpu& C = s.cpu(c);
                CpuView v = {};
                v.ax = C.ax;
                v.bx = C.bx;
                v.cx = C.cx;
                v.dx = C.dx;
                v.ip = C.ip;
                v.rh = C.rh;
                v.wh = C.wh;
                v.sp = C.sp;
                memcpy(v.stack, C.stack, sizeof v.stack);
                v.executed = C.executed;
                v.err = C.err;
                v.errs = C.errs;
                v.halted = C.halted;
                v.yielded = C.yielded;
                v.result = s.accel_result(c);
                v.pending = s.accel_pending(c);
                v.kind = s.cpu_kind(c);
                const double core = s.params().tick_units;
                v.share = core > 0 ? static_cast<float>(s.cpu_time(c) / core) : 0.0f;
                v.eshare = core > 0 ? static_cast<float>(s.cpu_etime(c) / core) : 0.0f;
                f->cpus.push(v);
            }
            // Phase 8: its warps' lanes, and which run in each warp's next step.
            u32 wn = 0;
            for (u32 wi = s.warp_head(sel_body); wi != kNone; wi = s.warp(wi).next, ++wn) {
                const u32 nl = s.lanes_per_warp();
                u32 minip = kNone;
                for (u32 l = 0; l < nl; ++l) {
                    const Lane& x = s.lane(wi, l);
                    if (!x.halted && !x.yielded && x.ip < minip)
                        minip = x.ip;
                }
                for (u32 l = 0; l < nl; ++l) {
                    const Lane& x = s.lane(wi, l);
                    LaneView v = {};
                    v.warp = wn;
                    v.lane = l;
                    v.ip = x.ip;
                    v.rh = x.rh;
                    v.wh = x.wh;
                    v.ax = x.ax;
                    v.bx = x.bx;
                    v.cx = x.cx;
                    v.dx = x.dx;
                    v.sp = x.sp;
                    v.err = x.err;
                    v.errs = x.errs;
                    v.halted = x.halted;
                    v.yielded = x.yielded;
                    v.next = !x.halted && !x.yielded && x.ip == minip ? 1 : 0;
                    v.wait = s.warp(wi).wait;
                    v.steps = s.warp(wi).steps;
                    f->lanes.push(v);
                }
            }
        }
    }

    // New since the previous frame: chart samples, events, genebank saves.
    f->clear_new();
    f->sample_tick.append(new_tick_.data(), new_tick_.size());
    f->sample.append(new_rows_.data(), new_rows_.size());
    new_tick_.clear();
    new_rows_.clear();
    if (events_sent_ < run_->event_base())
        events_sent_ = run_->event_base(); // dropped before this session saw them
    for (; events_sent_ < run_->event_count(); ++events_sent_) {
        const Str t = run_->event_text(events_sent_);
        ViewEvent ev = {};
        ev.tick = run_->event_tick(events_sent_);
        parse_event(t, &ev.kind, &ev.node);
        ev.off = static_cast<u32>(f->event_text.size());
        ev.len = static_cast<u32>(t.n);
        append_text(f->event_text, t);
        f->events.push(ev);
    }
    run_->drop_events(); // handed on: the run needn't hold them for hours
    if (w.model() == Model::soup) {
        // The newest kMaxSaves at most; the run's log is emptied once taken.
        const usize n = run_->genebank_count();
        for (usize i = n > kMaxSaves ? n - kMaxSaves : 0; i < n; ++i) {
            GenomeEntry e;
            entry_from_record(run_->genebank_record(i), run_->genebank_codons(), w.soup_params().table, &e,
                              f->save_text);
            f->saves.push(e);
        }
        run_->clear_genebank_log();
    }
}

// --- the archive -----------------------------------------------------------

namespace {

bool read_text(const char* dir, const char* name, Array<u8>* out) {
    TextBuf path;
    path.append(dir);
    path.append_char('/');
    path.append(name);
    return read_file(path.c_str(), out);
}

} // namespace

bool RunArchive::load(const char* dir, TextBuf* err) {
    config_.clear();
    nodes_ = 0;
    procedural_ = sent_ = false;
    win_tick_.clear();
    win_ticks_.clear();
    win_vals_.clear();
    edges_.clear();
    px_.clear();
    py_.clear();
    landscape_ = false;
    land_.clear();
    edge_phase_.clear();
    node_mass_.clear();
    masses_.clear();
    sample_tick_.clear();
    samples_.clear();
    events_.clear();
    event_text_.clear();
    saves_.clear();
    save_text_.clear();

    Array<u8> stats, nodes, replay, bank;
    const bool has_stats = read_text(dir, "stats.csv", &stats);
    const bool has_nodes = read_text(dir, "nodes.csv", &nodes);
    const bool has_replay = read_text(dir, "replay.log", &replay);
    const bool has_bank = read_text(dir, "genebank.txt", &bank);
    if (!has_stats && !has_nodes && !has_replay && !has_bank) {
        err->append("no stats.csv, nodes.csv, replay.log or genebank.txt in '");
        err->append(dir);
        err->append_char('\'');
        return false;
    }
    auto text = [](const Array<u8>& a) { return Str(reinterpret_cast<const char*>(a.data()), a.size()); };

    // The replay log: the config (for the graph) and the events.
    if (has_replay) {
        ReplayLog log;
        if (!log.parse(text(replay), err))
            return false;
        config_.append(log.config_text());
        for (usize i = 0; i < log.event_count(); ++i) {
            ViewEvent ev = {};
            ev.tick = log.event_tick(i);
            parse_event(log.event_text(i), &ev.kind, &ev.node);
            ev.off = static_cast<u32>(event_text_.size());
            ev.len = static_cast<u32>(log.event_text(i).n);
            append_text(event_text_, log.event_text(i));
            events_.push(ev);
        }
        // The graph: a world built from the config (accelerators and the
        // detector off: only its shape is wanted).
        Config cfg;
        Config::ParseError pe;
        if (log.replay_config(&cfg, &pe)) { // (the config it ran with: legacy defaults for a log before v3)
            cfg.set("accel.mode", "instant");
            cfg.set("gpu.warps", "false"); // (no GPU wanted either)
            cfg.set("detect.enabled", "false");
            // Only the graph is wanted: no disk tier, in files or RAM (nor
            // the archive, the disk tiers' second: docs/world-plan.md W4).
            cfg.set("world.disk", "0");
            cfg.set("world.archive", "0");
            cfg.set("disk.path", "");
            World* w = new World;
            if (w->init(cfg)) {
                nodes_ = w->node_count();
                procedural_ = w->spatial();
                const double spacing = 1.0 / sqrt(static_cast<double>(nodes_ ? nodes_ : 1));
                for (u32 e = 0; e < w->edge_count(); ++e) {
                    if (w->edge_src(e) >= w->edge_dst(e))
                        continue;
                    EdgeView ev = {};
                    ev.a = w->edge_src(e);
                    ev.b = w->edge_dst(e);
                    ev.bridge = w->edge_bridge(e) ? 1 : 0;
                    ev.long_link = !ev.bridge && procedural_ && w->edge_distance(e) > 2.5 * spacing ? 1 : 0;
                    edges_.push(ev);
                    edge_phase_.push(w->edge_phase(e));
                }
                map_positions(*w, &px_, &py_);
                if (w->landscape()) { // the land and its bridges' schedule
                    ViewFrame tmp;
                    land_view(*w, &tmp);
                    landscape_ = true;
                    land_w_ = tmp.land_w;
                    land_h_ = tmp.land_h;
                    land_ = move(tmp.land);
                    masses_ = move(tmp.masses);
                    memcpy(box_, tmp.box, sizeof box_);
                    open_every_ = w->land_params().open_every;
                    open_ticks_ = w->land_params().open_ticks;
                    node_mass_.resize(nodes_);
                    for (u32 n = 0; n < nodes_; ++n)
                        node_mass_[n] = w->node_mass(n);
                }
            }
            delete w;
        }
    }
    if (has_stats && !load_stats(text(stats), err))
        return false;
    if (has_nodes) {
        win_vals_.clear();
        if (!load_nodes(text(nodes), err))
            return false;
    }
    if (has_bank && !load_genebank(text(bank), err))
        return false;
    return true;
}

bool RunArchive::load_stats(Str t, TextBuf* err) {
    Str line;
    usize at = next_line(t, 0, &line);
    Str f[96];
    const usize nh = split_fields(line, f, 96);
    u32 col[kStatsColumns];
    for (u32 i = 0; i < kStatsColumns; ++i)
        col[i] = 0xFFFFFFFFu;
    for (usize i = 0; i < nh; ++i) {
        const u32 c = stats_column_index(f[i]);
        if (c < kStatsColumns)
            col[c] = static_cast<u32>(i);
    }
    if (col[1] == 0xFFFFFFFFu || col[2] == 0xFFFFFFFFu) {
        err->append("stats.csv: not a soup-model stats file (no tick_last / ticks)");
        return false;
    }
    while (at < t.n) {
        at = next_line(t, at, &line);
        if (line.n == 0)
            continue;
        const usize nf = split_fields(line, f, 96);
        StatsRow r = {};
        for (u32 i = 0; i < kStatsIntColumns; ++i)
            r.v[i] = static_cast<u64>(field_f64(f, nf, col[i]));
        r.compress = field_f64(f, nf, col[kStatsIntColumns]);
        r.ambient = field_f64(f, nf, col[kStatsIntColumns + 1]);
        double row[kStatsColumns];
        chart_row(r, row);
        sample_tick_.push(r.v[1] + 1);
        samples_.append(row, kStatsColumns);
    }
    return true;
}

bool RunArchive::load_nodes(Str t, TextBuf* err) {
    Str line;
    usize at = next_line(t, 0, &line);
    Str f[64];
    const usize nh = split_fields(line, f, 64);
    u32 col[kNcCount];
    for (u32 c = 0; c < kNcCount; ++c) {
        col[c] = 0xFFFFFFFFu;
        for (usize i = 0; i < nh; ++i)
            if (f[i].eq(kNodeCols[c]))
                col[c] = static_cast<u32>(i);
    }
    if (col[kNcLast] == 0xFFFFFFFFu || col[kNcNode] == 0xFFFFFFFFu) {
        err->append("nodes.csv: no tick_last / node columns");
        return false;
    }
    // Rows come window by window, node by node. Each window keeps every node's
    // columns as doubles (windows x nodes x kNcCount); the first window sets
    // the node count.
    u32 max_node = 0;
    u64 cur_last = ~0ull;
    double row[kNcCount];
    while (at < t.n) {
        at = next_line(t, at, &line);
        if (line.n == 0)
            continue;
        const usize nf = split_fields(line, f, 64);
        for (u32 c = 0; c < kNcCount; ++c)
            row[c] = field_f64(f, nf, col[c]);
        const u64 last = static_cast<u64>(row[kNcLast]);
        const u32 node = static_cast<u32>(row[kNcNode]);
        if (last != cur_last) {
            cur_last = last;
            win_tick_.push(last + 1);
            win_ticks_.push(static_cast<u32>(last + 1 - static_cast<u64>(row[kNcFirst])));
        }
        const usize w = win_tick_.size() - 1;
        if (w == 0) {
            if (node + 1 > max_node)
                max_node = node + 1;
        } else if (node >= max_node) {
            err->append("nodes.csv: a node appears that the first window lacks");
            return false;
        }
        const usize need = (w + 1) * static_cast<usize>(max_node) * kNcCount;
        if (win_vals_.size() < need)
            win_vals_.resize(need);
        memcpy(win_vals_.data() + (w * max_node + node) * kNcCount, row, sizeof row);
    }
    if (nodes_ == 0)
        nodes_ = max_node;
    if (max_node != nodes_) {
        err->append("nodes.csv: its node count does not match the run's config");
        return false;
    }
    if (!procedural_) {
        // All positions 0 means a ring world: lay it out on a circle.
        bool any = false;
        for (usize i = 0; i < win_vals_.size() / kNcCount && !any; ++i)
            any = win_vals_[i * kNcCount + kNcX] != 0 || win_vals_[i * kNcCount + kNcY] != 0;
        procedural_ = any;
    }
    return true;
}

bool RunArchive::load_genebank(Str t, TextBuf* err) {
    (void)err;
    // "<name> tick=.. node=.. lineage=n.k parent=<name|unsaved|none> hash=.. parent_hash=..|-"
    // then "  <op names>". Only the newest kMaxSaves are kept.
    Str line;
    usize at = 0, entries = 0;
    while (at < t.n) {
        at = next_line(t, at, &line);
        if (line.n && line.p[0] != '#' && line.p[0] != ' ')
            ++entries;
    }
    usize skip = entries > kMaxSaves ? entries - kMaxSaves : 0;
    at = 0;
    while (at < t.n) {
        at = next_line(t, at, &line);
        if (line.n == 0 || line.p[0] == '#' || line.p[0] == ' ')
            continue;
        if (skip) {
            --skip;
            continue;
        }
        GenomeEntry e;
        memset(&e, 0, sizeof e);
        const usize sp = line.find(' ');
        copy_name(e.name, line.sub(0, sp < line.n ? sp : line.n));
        u64 v = 0;
        if (parse_u64(kv(line, "tick"), &v))
            e.tick = v;
        if (parse_u64(kv(line, "node"), &v))
            e.node = static_cast<u32>(v);
        parse_lineage(kv(line, "lineage"), &e.lineage);
        copy_name(e.parent_name, kv(line, "parent"));
        const Str h = kv(line, "hash"), ph = kv(line, "parent_hash");
        TextBuf hx;
        hx.append("0x");
        hx.append(h);
        parse_u64(hx.view(), &e.genotype);
        if (!ph.eq("-")) {
            hx.clear();
            hx.append("0x");
            hx.append(ph);
            parse_u64(hx.view(), &e.parent);
        }
        // The length is the name's leading digits.
        u32 len = 0;
        for (usize i = 0; i < 15 && e.name[i] >= '0' && e.name[i] <= '9'; ++i)
            len = len * 10 + static_cast<u32>(e.name[i] - '0');
        e.len = len;
        Str genome;
        if (at < t.n) {
            Str g;
            const usize after = next_line(t, at, &g);
            if (g.n && g.p[0] == ' ') {
                genome = g.trim();
                at = after;
            }
        }
        e.off = static_cast<u32>(save_text_.size());
        append_text(save_text_, genome);
        e.n = static_cast<u32>(genome.n);
        saves_.push(e);
    }
    return true;
}

void RunArchive::build(u32 window, const ViewRequest& q, ViewFrame* f) {
    (void)q;
    f->archive = true;
    f->hashed = false;
    f->hash = 0;
    f->instructions = 0;
    f->detect_tick = 0;
    f->procedural = procedural_;
    f->nodes = nodes_;
    f->knobs = 0;
    f->sel_node = q.node < nodes_ ? q.node : kNone;
    f->strip.clear();
    f->strip_owner.clear();
    f->strip_w = f->strip_h = 0;
    f->bodies.clear();
    f->sel_body = kNone;
    f->req_body = q.body;
    f->req_serial = q.serial;
    f->body_ok = false;
    f->genome.clear();
    f->cpus.clear();
    f->cargo.clear();
    f->node.resize(nodes_);
    const bool have = window < windows();
    f->tick = have ? win_tick_[window] : (sample_tick_.size() ? sample_tick_.back() : 0);
    const double kPi = 3.14159265358979323846;
    for (u32 n = 0; n < nodes_; ++n) {
        NodeView& v = f->node[n];
        memset(&v, 0, sizeof v);
        if (n < px_.size()) {
            v.x = px_[n];
            v.y = py_[n];
        } else {
            const double a = 2 * kPi * n / (nodes_ ? nodes_ : 1) - kPi / 2;
            v.x = static_cast<float>(0.5 + 0.45 * cos(a));
            v.y = static_cast<float>(0.5 + 0.45 * sin(a));
        }
        v.live = 1;
        v.health = 100;
        if (!have)
            continue;
        const double* r = win_vals_.data() + (static_cast<usize>(window) * nodes_ + n) * kNcCount;
        const double ticks = win_ticks_[window] ? win_ticks_[window] : 1;
        if (procedural_ && n >= px_.size()) { // rounded in the file: the config's world is exact
            v.x = static_cast<float>(r[kNcX]);
            v.y = static_cast<float>(r[kNcY]);
        }
        v.degree = static_cast<u32>(r[kNcDegree]);
        v.soup_size = static_cast<u32>(r[kNcSize]);
        v.millicores = static_cast<u32>(r[kNcMilli]);
        v.gpus = static_cast<u32>(r[kNcGpus]);
        v.npus = static_cast<u32>(r[kNcNpus]);
        v.cool = static_cast<float>(r[kNcCool]);
        v.spark_rate = static_cast<float>(r[kNcSpark]);
        v.radiation = static_cast<float>(r[kNcRad]);
        v.bodies = static_cast<u32>(r[kNcBodies]);
        v.waste = static_cast<u32>(r[kNcWaste]);
        v.free_millicores = static_cast<u32>(r[kNcFree]);
        v.births = static_cast<float>(r[kNcBirths] / ticks);
        v.deaths = static_cast<float>(r[kNcDeaths] / ticks);
        v.sent = static_cast<float>(r[kNcSent] / ticks);
        v.arrived = static_cast<float>(r[kNcArrived] / ticks);
        v.lineages = static_cast<u32>(r[kNcLineages]);
        v.colonists = static_cast<u32>(r[kNcColonists]);
        v.live = r[kNcLive] != 0 ? 1 : 0;
        v.health = static_cast<u32>(r[kNcHealth]);
        v.life = static_cast<u32>(r[kNcLife]);
        v.known = (1u << kColorBodies) | (1u << kColorBirths) | (1u << kColorColonists) | (1u << kColorHealth) |
                  (1u << kColorFreeCpu) | (1u << kColorWaste) | (1u << kColorMemory) | (1u << kColorMillicores) |
                  (1u << kColorClimate) | (1u << kColorSparks) | (1u << kColorRadiation);
    }
    for (u32 n = 0; n < nodes_ && n < node_mass_.size(); ++n)
        f->node[n].mass = node_mass_[n];
    f->edge.clear();
    for (usize i = 0; i < edges_.size(); ++i) {
        EdgeView e = edges_[i];
        if (e.bridge) // shut or open at the window's tick, by its schedule
            e.shut = bridge_open(f->tick, open_every_, open_ticks_, edge_phase_[i]) ? 0 : 1;
        if (e.a < nodes_ && e.b < nodes_ && f->node[e.a].live && f->node[e.b].live)
            f->edge.push(e);
    }
    f->landscape = landscape_;
    f->land_sent = false;
    f->land.clear();
    f->masses.clear();
    f->clear_new();
    if (!sent_) {
        if (landscape_) {
            f->land_sent = true;
            f->land_w = land_w_;
            f->land_h = land_h_;
            f->land.append(land_.data(), land_.size());
            f->masses.append(masses_.data(), masses_.size());
            memcpy(f->box, box_, sizeof box_);
        }
        sent_ = true;
        f->sample_tick.append(sample_tick_.data(), sample_tick_.size());
        f->sample.append(samples_.data(), samples_.size());
        f->events.append(events_.data(), events_.size());
        f->event_text.append(event_text_.data(), event_text_.size());
        f->saves.append(saves_.data(), saves_.size());
        f->save_text.append(save_text_.data(), save_text_.size());
    }
}

} // namespace nova
