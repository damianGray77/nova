// A run's scores for the settings search (docs/analysis-plan.md A1), from its
// own outputs: lineages.csv, genebank.txt, stats.csv and result.txt.
#include <cmath> // log

#include "nova/analysis.h"
#include "nova/io.h"

namespace nova {

const char* score_name(u32 i) {
    static const char* const kNames[kScoreCount] = {"lifespan",  "depth",       "genotypes", "evenness",
                                                    "novelty",   "persistence", "speed"};
    return i < kScoreCount ? kNames[i] : "?";
}

namespace {

// Nearest-rank percentile q (0-1] of v's values (sorted here).
double percentile(Array<u64>& v, double q) {
    if (v.empty())
        return 0;
    sort_u64(v.data(), v.size());
    usize r = static_cast<usize>(q * static_cast<double>(v.size()) + 0.999999);
    if (r < 1)
        r = 1;
    if (r > v.size())
        r = v.size();
    return static_cast<double>(v[r - 1]);
}

struct Span {
    u64 founded, ended;
    u32 depth, pad;
};

// result.txt's `key = value` lines (values may hold anything, so not Config).
Str result_value(Str text, const char* key) {
    const Str k(key);
    usize i = 0;
    while (i < text.n) {
        usize e = i;
        while (e < text.n && text.p[e] != '\n')
            ++e;
        Str line = text.sub(i, e - i);
        i = e + 1;
        const usize eq = line.find('=');
        if (eq < line.n && line.sub(0, eq).trim().eq(k))
            return line.sub(eq + 1, line.n - eq - 1).trim();
    }
    return Str();
}

} // namespace

bool score_run(const char* dir, const ScoreOptions& o, RunScores* out, TextBuf* err) {
    *out = RunScores{};
    TextBuf path;

    // result.txt: the end tick, the time and the hash.
    path_join(path, dir, "result.txt");
    Array<u8> bytes;
    double seconds = 0;
    if (read_file(path.c_str(), &bytes)) {
        const Str text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        parse_u64(result_value(text, "end_tick"), &out->ticks);
        parse_f64(result_value(text, "seconds"), &seconds);
        parse_hex(result_value(text, "hash"), &out->hash);
    }
    out->seconds = seconds;

    Config cfg;
    TextBuf cerr;
    const bool have_cfg = load_run_config(dir, &cfg, &cerr);
    const double nodes = have_cfg ? static_cast<double>(key_peek_u64(cfg, "world.nodes")) : 1.0;
    out->body_floor = o.body_floor > 0 ? o.body_floor : nodes;

    // stats.csv: persistence, and the end tick if result.txt had none.
    path.clear();
    path_join(path, dir, "stats.csv");
    LineReader r;
    Str line;
    if (r.open(path.c_str())) {
        CsvHeader h;
        if (r.next(&line) && h.parse(line)) {
            const i32 cb = h.find("bodies"), cl = h.find("tick_last");
            Str f[256];
            u64 rows = 0, held = 0;
            double minb = -1;
            u64 last = 0;
            while (r.next(&line)) {
                const u32 nf = csv_split(line, f, 256);
                double b = 0;
                if (cb >= 0 && static_cast<u32>(cb) < nf && parse_f64(f[cb], &b)) {
                    ++rows;
                    held += b >= out->body_floor;
                    if (minb < 0 || b < minb)
                        minb = b;
                }
                u64 tl = 0;
                if (cl >= 0 && static_cast<u32>(cl) < nf && parse_u64(f[cl], &tl))
                    last = tl + 1;
            }
            out->s[kScorePersistence] = rows ? static_cast<double>(held) / static_cast<double>(rows) : 0.0;
            out->min_bodies = minb < 0 ? 0 : minb;
            if (out->ticks == 0)
                out->ticks = last;
        }
        r.close();
    }
    out->s[kScoreSpeed] = seconds > 0 ? static_cast<double>(out->ticks) / seconds : 0.0;

    // The gated lineages, then their spans over every node they lived on.
    GatedSet gated;
    if (!load_gated(dir, &gated, err))
        return false;
    out->gated = gated.ids.size();
    path.clear();
    path_join(path, dir, "lineages.csv");
    if (gated.csv && r.open(path.c_str())) {
        CsvHeader h;
        if (r.next(&line) && h.parse(line)) {
            const i32 ci = h.find("lineage"), cf = h.find("founded"), ce = h.find("ended"), cd = h.find("max_depth");
            if (ci < 0 || cf < 0 || ce < 0 || cd < 0) {
                err->append("lineages.csv lacks lineage, founded, ended or max_depth");
                return false;
            }
            i32 need = ci;
            if (cf > need)
                need = cf;
            if (ce > need)
                need = ce;
            if (cd > need)
                need = cd;
            HashMap<Span> spans;
            Str f[64];
            while (r.next(&line)) {
                if (csv_split(line, f, static_cast<u32>(need + 1)) < static_cast<u32>(need + 1))
                    continue;
                u64 id = 0, a = 0, b = 0, dep = 0;
                if (!parse_lineage_id(f[ci], &id) || !gated.contains(id) || !parse_u64(f[cf], &a) ||
                    !parse_u64(f[ce], &b) || !parse_u64(f[cd], &dep))
                    continue;
                bool fresh = false;
                Span& s = spans.get_or_insert(id, &fresh);
                if (fresh) {
                    s.founded = a;
                    s.ended = b;
                    s.depth = static_cast<u32>(dep);
                } else {
                    s.founded = a < s.founded ? a : s.founded;
                    s.ended = b > s.ended ? b : s.ended;
                    s.depth = static_cast<u32>(dep) > s.depth ? static_cast<u32>(dep) : s.depth;
                }
            }
            Array<u64> life, depth;
            for (usize i = 0; i < spans.slot_count(); ++i) {
                if (!spans.slot_used(i))
                    continue;
                const Span& s = spans.slot_value(i);
                life.push(s.ended > s.founded ? s.ended - s.founded : 0);
                depth.push(s.depth);
            }
            out->s[kScoreLifespan] = percentile(life, 0.5);
            out->s[kScoreDepth] = percentile(depth, 0.9);
        }
        r.close();
    }

    // genebank.txt: distinct genotypes, their spread over lineages, novelty.
    path.clear();
    path_join(path, dir, "genebank.txt");
    GenebankReader bank;
    if (bank.open(path.c_str())) {
        CodonTable table;
        if (!run_codon_table(cfg, &table, err))
            return false;
        OpAlphabet abc;
        abc.init(table);
        NoveltyOptions no;
        no.threshold = o.novelty_threshold;
        no.top = 0;
        const u64 half = out->ticks ? (out->ticks + 1) / 2 : 1;
        no.window = half;
        NoveltyMeter meter;
        meter.init(abc.size(), no);
        HashMap<u32> per_lineage;
        BankEntry e;
        Array<u8> sym;
        while (bank.next(&e)) {
            ++out->saves;
            if (!abc.encode(e.genome, &sym) || sym.empty())
                continue;
            const double s = meter.add(e.tick, e.hash, sym.data(), static_cast<u32>(sym.size()),
                                       gated.contains(e.lineage));
            if (s >= 0)
                ++per_lineage.get_or_insert(e.lineage);
        }
        meter.finish();
        const NoveltyWindow& t = meter.total();
        out->s[kScoreGenotypes] = static_cast<double>(t.fresh);
        double hsum = 0;
        u64 lineages = 0;
        for (usize i = 0; i < per_lineage.slot_count(); ++i) {
            if (!per_lineage.slot_used(i))
                continue;
            const double p = static_cast<double>(per_lineage.slot_value(i)) / static_cast<double>(t.fresh);
            hsum -= p * log(p);
            ++lineages;
        }
        out->s[kScoreEvenness] = lineages > 1 ? hsum / log(static_cast<double>(lineages)) : 0.0;
        double late = 0;
        for (usize i = 0; i < meter.windows().size(); ++i)
            if (meter.windows()[i].first >= half)
                late += meter.windows()[i].success_surprise;
        const u64 late_ticks = out->ticks > half ? out->ticks - half : 0;
        out->s[kScoreNovelty] = late_ticks ? late * 1000.0 / static_cast<double>(late_ticks) : 0.0;
    }
    return true;
}

} // namespace nova
