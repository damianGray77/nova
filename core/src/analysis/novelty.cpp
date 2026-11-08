// The novelty and plateau meter, offline (docs/analysis-plan.md A2): every
// genotype the genebank saved, in save order, through a 4-codon n-gram model.
#include <cstdio> // snprintf

#include "nova/analysis.h"

namespace nova {

// --- the meter ------------------------------------------------------------------------

bool NoveltyMeter::init(u32 alphabet, const NoveltyOptions& o) {
    o_ = o;
    if (o_.window == 0)
        o_.window = 1;
    win_.clear();
    seen_.clear();
    cur_ = NoveltyWindow{};
    total_ = NoveltyWindow{};
    memset(hist_, 0, sizeof hist_);
    open_ = false;
    return model_.init(alphabet, o.order);
}

double NoveltyMeter::add(u64 tick, u64 hash, const u8* sym, u32 len, bool successful) {
    const u64 w = tick / o_.window;
    if (!open_) {
        cur_ = NoveltyWindow{};
        cur_.first = w * o_.window;
        cur_.last = cur_.first + o_.window - 1;
        open_ = true;
    }
    while (w > cur_.first / o_.window) { // close this window (and any empty ones after it)
        win_.push(cur_);
        const u64 next = cur_.first + o_.window;
        cur_ = NoveltyWindow{};
        cur_.first = next;
        cur_.last = next + o_.window - 1;
    }
    ++cur_.saves;
    ++total_.saves;
    bool fresh = false;
    seen_.get_or_insert(hash, &fresh);
    if (!fresh)
        return -1;
    const double s = model_.surprise(sym, len);
    model_.learn(sym, len);
    const bool novel = s >= o_.threshold;
    const u32 bin = !(s > 0) ? 0 : (s >= 9.5 ? kBins - 1 : static_cast<u32>(s * 2)); // (NaN-safe)
    ++hist_[0][bin];
    hist_[1][bin] += successful;
    NoveltyWindow* ws[2] = {&cur_, &total_};
    for (NoveltyWindow* x : ws) {
        ++x->fresh;
        x->surprise += s;
        x->novel += novel;
        if (successful) {
            ++x->successful;
            x->success_surprise += s;
            x->novel_success += novel;
        }
    }
    return s;
}

void NoveltyMeter::finish() {
    if (open_)
        win_.push(cur_);
    open_ = false;
    if (!win_.empty()) {
        total_.first = win_[0].first;
        total_.last = win_.back().last;
    }
}

// --- a run folder ------------------------------------------------------------------------

namespace {

struct TopItem {
    double surprise;
    u64 tick, lineage;
    u32 node, len;
    u32 off, n; // genome text in the arena
    char name[16];
};

void flush_top(Array<TopItem>& top, Array<char>& arena, u64 window_first, TextBuf& out) {
    // Largest surprise first; ties by save order (insertion sort is stable).
    for (usize i = 1; i < top.size(); ++i) {
        const TopItem t = top[i];
        usize j = i;
        while (j > 0 && top[j - 1].surprise < t.surprise) {
            top[j] = top[j - 1];
            --j;
        }
        top[j] = t;
    }
    char num[32];
    for (usize i = 0; i < top.size(); ++i) {
        const TopItem& t = top[i];
        out.append_u64(window_first);
        out.append_char(',');
        out.append_u64(i + 1);
        out.append_char(',');
        snprintf(num, sizeof num, "%.4f", t.surprise);
        out.append(num);
        out.append_char(',');
        out.append(t.name);
        out.append_char(',');
        out.append_u64(t.node);
        out.append_char(',');
        append_lineage_id(out, t.lineage);
        out.append_char(',');
        out.append_u64(t.tick);
        out.append_char(',');
        out.append_u64(t.len);
        out.append_char(',');
        out.append(Str(arena.data() + t.off, t.n));
        out.append_char('\n');
    }
    top.clear();
    arena.clear();
}

} // namespace

bool novelty_analyse(const char* dir, const NoveltyOptions& o, NoveltyReport* out, TextBuf* err) {
    Config cfg;
    TextBuf cerr;
    if (!load_run_config(dir, &cfg, &cerr))
        cfg.clear(); // unknown: the registry's default table
    CodonTable table;
    if (!run_codon_table(cfg, &table, err))
        return false;
    memcpy(out->table, table.name, sizeof out->table);
    out->table[sizeof out->table - 1] = '\0';
    OpAlphabet abc;
    abc.init(table);
    out->alphabet = abc.size();

    GatedSet gated;
    if (!load_gated(dir, &gated, err))
        return false;
    if (o.as_live && !gated.replay) {
        err->append("--as-live needs the run's replay.log (when each lineage met the gate)");
        return false;
    }
    out->gated = gated.ids.size();
    out->csv = gated.csv;
    out->replay = gated.replay;

    TextBuf path;
    path_join(path, dir, "genebank.txt");
    GenebankReader bank;
    if (!bank.open(path.c_str())) {
        err->append("cannot read '");
        err->append(path.view());
        err->append_char('\'');
        return false;
    }
    NoveltyMeter meter;
    if (!meter.init(abc.size(), o)) {
        err->append("the novelty model refused its alphabet");
        return false;
    }
    out->top_csv.clear();
    out->top_csv.append("window_first,rank,surprise,name,node,lineage,tick,len,genome\n");
    Array<TopItem> top;
    Array<char> arena;
    u64 top_window = ~0ull;
    const u64 window = o.window ? o.window : 1;
    BankEntry e;
    Array<u8> sym;
    out->skipped = 0;
    while (bank.next(&e)) {
        if (!abc.encode(e.genome, &sym) || sym.empty()) {
            ++out->skipped;
            continue;
        }
        const u64 wf = e.tick / window * window;
        // Successful: met the gate at any time, or (as_live) by the window's end.
        const bool ok = o.as_live ? gated.gated_by(e.lineage, wf + window - 1) : gated.contains(e.lineage);
        if (o.top && wf != top_window) {
            if (top_window != ~0ull)
                flush_top(top, arena, top_window, out->top_csv);
            top_window = wf;
        }
        const double s = meter.add(e.tick, e.hash, sym.data(), static_cast<u32>(sym.size()), ok);
        if (s < 0 || !ok || !o.top)
            continue;
        usize slot = top.size();
        if (top.size() >= o.top) {
            usize low = 0;
            for (usize i = 1; i < top.size(); ++i)
                if (top[i].surprise < top[low].surprise)
                    low = i;
            if (top[low].surprise >= s)
                continue;
            slot = low;
        } else {
            top.push(TopItem{});
        }
        TopItem& t = top[slot];
        t.surprise = s;
        t.tick = e.tick;
        t.lineage = e.lineage;
        t.node = e.node;
        t.len = static_cast<u32>(sym.size());
        t.off = static_cast<u32>(arena.size());
        t.n = static_cast<u32>(e.genome.n);
        arena.append(e.genome.p, e.genome.n);
        const usize nn = e.name.n < sizeof t.name - 1 ? e.name.n : sizeof t.name - 1;
        memcpy(t.name, e.name.p, nn);
        t.name[nn] = '\0';
    }
    if (bank.error()[0]) {
        err->append(bank.error());
        return false;
    }
    if (o.top && top_window != ~0ull)
        flush_top(top, arena, top_window, out->top_csv);
    meter.finish();
    out->windows.clear();
    out->windows.append(meter.windows().data(), meter.windows().size());
    out->total = meter.total();
    memcpy(out->hist[0], meter.histogram(0), sizeof out->hist[0]);
    memcpy(out->hist[1], meter.histogram(1), sizeof out->hist[1]);
    return true;
}

void novelty_csv(const NoveltyReport& r, TextBuf& out) {
    out.append("window_first,window_last,saves,fresh,innovation,novel_share,novel,successful,"
               "success_innovation,novel_success\n");
    char num[48];
    for (usize i = 0; i < r.windows.size(); ++i) {
        const NoveltyWindow& w = r.windows[i];
        out.append_u64(w.first);
        out.append_char(',');
        out.append_u64(w.last);
        out.append_char(',');
        out.append_u64(w.saves);
        out.append_char(',');
        out.append_u64(w.fresh);
        out.append_char(',');
        if (w.fresh) {
            snprintf(num, sizeof num, "%.4f,%.4f", w.innovation(), w.novel_share());
            out.append(num);
        } else {
            out.append(",");
        }
        out.append_char(',');
        out.append_u64(w.novel);
        out.append_char(',');
        out.append_u64(w.successful);
        out.append_char(',');
        if (w.successful) {
            snprintf(num, sizeof num, "%.4f", w.success_innovation());
            out.append(num);
        }
        out.append_char(',');
        out.append_u64(w.novel_success);
        out.append_char('\n');
    }
}

namespace {

// Means over windows [a, b) of one measure (only windows where it is defined).
struct Measure {
    const char* name;
    double (*get)(const NoveltyWindow&);
    bool (*defined)(const NoveltyWindow&);
};

double m_innov(const NoveltyWindow& w) {
    return w.innovation();
}
double m_share(const NoveltyWindow& w) {
    return w.novel_share();
}
double m_succ(const NoveltyWindow& w) {
    return w.success_innovation();
}
double m_nsucc(const NoveltyWindow& w) {
    return static_cast<double>(w.novel_success);
}
double m_fresh(const NoveltyWindow& w) {
    return static_cast<double>(w.fresh);
}
bool d_fresh(const NoveltyWindow& w) {
    return w.fresh > 0;
}
bool d_succ(const NoveltyWindow& w) {
    return w.successful > 0;
}
bool d_any(const NoveltyWindow&) {
    return true;
}

double mean_of(const Array<NoveltyWindow>& ws, usize a, usize b, const Measure& m) {
    double s = 0;
    u64 n = 0;
    for (usize i = a; i < b; ++i)
        if (m.defined(ws[i])) {
            s += m.get(ws[i]);
            ++n;
        }
    return n ? s / static_cast<double>(n) : 0.0;
}

// Least-squares slope of a measure against the window's first tick, per 100,000 ticks.
double slope_of(const Array<NoveltyWindow>& ws, usize a, usize b, const Measure& m) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    u64 n = 0;
    for (usize i = a; i < b; ++i) {
        if (!m.defined(ws[i]))
            continue;
        const double x = static_cast<double>(ws[i].first) / 100000.0, y = m.get(ws[i]);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++n;
    }
    const double dn = static_cast<double>(n);
    const double den = dn * sxx - sx * sx;
    return n > 1 && den != 0 ? (dn * sxy - sx * sy) / den : 0.0;
}

} // namespace

void novelty_summary(const NoveltyReport& r, TextBuf& out) {
    char line[256];
    const NoveltyWindow& t = r.total;
    snprintf(line, sizeof line,
             "%llu genebank entries, %llu new genotypes (%llu repeats on other nodes, %llu unreadable); "
             "table %s (%u ops); %llu gated lineages known (%s%s%s)\n",
             static_cast<unsigned long long>(t.saves + r.skipped), static_cast<unsigned long long>(t.fresh),
             static_cast<unsigned long long>(t.saves - t.fresh), static_cast<unsigned long long>(r.skipped),
             r.table, r.alphabet, static_cast<unsigned long long>(r.gated), r.csv ? "lineages.csv" : "",
             r.csv && r.replay ? " and " : "", r.replay ? "replay.log" : "");
    out.append(line);
    snprintf(line, sizeof line,
             "overall: innovation %.3f bits/codon, novel share %.3f, %llu successful (%.3f bits/codon), "
             "%llu novel and successful\n",
             t.innovation(), t.novel_share(), static_cast<unsigned long long>(t.successful), t.success_innovation(),
             static_cast<unsigned long long>(t.novel_success));
    out.append(line);
    // Where the threshold sits: new genotypes by surprise.
    for (u32 k = 0; k < 2; ++k) {
        out.append(k ? "  successful:   " : "surprise, all: ");
        const u64 total = k ? t.successful : t.fresh;
        for (u32 b = 0; b < NoveltyMeter::kBins; ++b) {
            const double share = total ? 100.0 * static_cast<double>(r.hist[k][b]) / static_cast<double>(total) : 0.0;
            if (b + 1 < NoveltyMeter::kBins)
                snprintf(line, sizeof line, " %.1f:%.1f%%", b * 0.5, share);
            else
                snprintf(line, sizeof line, " 9.5+:%.1f%%", share);
            out.append(line);
        }
        out.append(" (bits/codon: share)\n");
    }
    const usize n = r.windows.size();
    if (n < 4) {
        out.append("too few windows for a trend\n");
        return;
    }
    const Measure ms[] = {{"innovation (bits/codon)", m_innov, d_fresh},
                          {"novel share", m_share, d_fresh},
                          {"successful innovation (bits/codon)", m_succ, d_succ},
                          {"novel and successful per window", m_nsucc, d_any},
                          {"new genotypes per window", m_fresh, d_any}};
    const usize q[5] = {0, n / 4, n / 2, 3 * n / 4, n};
    snprintf(line, sizeof line, "%-36s %9s %9s %9s %9s   %s\n", "by quarter of the run", "Q1", "Q2", "Q3", "Q4",
             "slope/100k ticks, 2nd half");
    out.append(line);
    for (const Measure& m : ms) {
        snprintf(line, sizeof line, "%-36s %9.3f %9.3f %9.3f %9.3f   %+.4f\n", m.name, mean_of(r.windows, q[0], q[1], m),
                 mean_of(r.windows, q[1], q[2], m), mean_of(r.windows, q[2], q[3], m), mean_of(r.windows, q[3], q[4], m),
                 slope_of(r.windows, q[2], q[4], m));
        out.append(line);
    }
    const double q3 = mean_of(r.windows, q[2], q[3], ms[0]), q4 = mean_of(r.windows, q[3], q[4], ms[0]);
    const double s2 = slope_of(r.windows, q[2], q[4], ms[0]);
    const char* verdict = (q4 < q3 * 0.95 && s2 < 0) ? "falling" : (q4 > q3 * 1.05 && s2 > 0 ? "rising" : "flat");
    snprintf(line, sizeof line,
             "innovation in the second half is %s (Q3 %.3f -> Q4 %.3f bits/codon). The model starts empty, so the "
             "first windows are high by construction.\n",
             verdict, q3, q4);
    out.append(line);
}

} // namespace nova
