// The settings search, an autopilot for tuning (docs/analysis-plan.md A1).
//
// Each trial is ordinary runs (one per seed) of the base config with the
// trial's settings, run as `nova run run.cfg` child processes in the trial's
// own folder: a fatal error in one world (NOVA_FATAL ends a process) costs
// that trial, not the search; nothing process-wide (the fallback defaults,
// the registry's hooks, the GPU device, the priority) is shared between
// trials; and the command that ran a trial is the command that repeats it.
//
// The search's own choices come from search.seed alone: a Latin hypercube
// first, then batches picked by expected improvement under a Gaussian process
// (a batch's later members believe the model's prediction for the earlier
// ones). A batch is chosen only once the one before it has finished, so how
// fast the runs go never changes what is tried, and the same search file
// repeats exactly (unless score.speed is weighted: speed is the clock's).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>  // exp, log, floor, sqrt, cos
#include <cstdio> // printf, snprintf
#include <cstdlib> // strtod

#include "nova/analysis.h"
#include "nova/hash.h"
#include "nova/io.h"
#include "nova/rng.h"

namespace nova {

namespace {

bool score_index(Str name, u32* out) {
    for (u32 i = 0; i < kScoreCount; ++i)
        if (name.eq(score_name(i))) {
            *out = i;
            return true;
        }
    return false;
}

// Keys the search sets itself.
bool reserved_key(Str k) {
    return k.eq("seed") || k.starts_with("run.") || k.starts_with("output.") || k.starts_with("replay.");
}

u32 split_words(Str s, Str* out, u32 max) {
    u32 n = 0;
    usize i = 0;
    while (i < s.n && n < max) {
        while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t'))
            ++i;
        usize j = i;
        while (j < s.n && s.p[j] != ' ' && s.p[j] != '\t')
            ++j;
        if (j > i)
            out[n++] = Str(s.p + i, j - i);
        i = j;
    }
    return n;
}

u32 option_count(const Key& k) {
    Str w[64];
    return split_words(Str(k.options), w, 64);
}

Str option_at(const Key& k, u32 i) {
    Str w[64];
    const u32 n = split_words(Str(k.options), w, 64);
    return i < n ? w[i] : Str();
}

void fail_key(TextBuf* err, Str key, const char* what) {
    err->append(key);
    err->append(": ");
    err->append(what);
}

bool parse_range(Str name, Str value, SearchRange* r, TextBuf* err) {
    const Key* k = key_find(name);
    if (!k) {
        fail_key(err, name, "is not a setting (nova keys lists them)");
        return false;
    }
    if (reserved_key(name)) {
        fail_key(err, name, "is the search's own to set");
        return false;
    }
    r->key = k;
    r->log = false;
    Str w[4];
    const u32 nw = split_words(value, w, 4);
    if (k->kind == KeyKind::text) {
        fail_key(err, name, "is text: only switches, whole numbers, numbers and choices can be searched");
        return false;
    }
    if (k->kind == KeyKind::flag || k->kind == KeyKind::choice) {
        if (nw > 1 || (nw == 1 && !w[0].eq("registry"))) {
            fail_key(err, name, "a switch or a choice is searched over all its values: give 'registry' or nothing");
            return false;
        }
        r->lo = 0;
        r->hi = k->kind == KeyKind::flag ? 1 : option_count(*k);
        return true;
    }
    u32 at = 0;
    if (nw >= 1 && w[0].eq("registry")) {
        r->lo = k->lo;
        r->hi = k->hi;
        at = 1;
        if (k->hi >= kKeyNoMax) {
            fail_key(err, name, "its registry range has no upper limit: give one (range.x = lo hi)");
            return false;
        }
        if (k->flags & kKeyOpenMin) {
            fail_key(err, name, "its registry range is open at the bottom: give one (range.x = lo hi)");
            return false;
        }
    } else {
        if (nw < 2 || !parse_f64(w[0], &r->lo) || !parse_f64(w[1], &r->hi)) {
            fail_key(err, name, "expected 'lo hi [log|linear]' or 'registry [log|linear]'");
            return false;
        }
        at = 2;
        const bool lo_ok = (k->flags & kKeyOpenMin) ? r->lo > k->lo : r->lo >= k->lo;
        if (!lo_ok || r->hi > k->hi || !(r->lo < r->hi)) {
            char what[192];
            snprintf(what, sizeof what, "the range must lie inside the registry's (%g to %g) with lo below hi", k->lo,
                     k->hi);
            fail_key(err, name, what);
            return false;
        }
        if (k->kind == KeyKind::whole && (r->lo != floor(r->lo) || r->hi != floor(r->hi))) {
            fail_key(err, name, "is a whole number: give whole bounds");
            return false;
        }
    }
    r->log = (k->flags & kKeyLog) != 0;
    if (at < nw) {
        if (w[at].eq("log"))
            r->log = true;
        else if (w[at].eq("linear"))
            r->log = false;
        else {
            fail_key(err, name, "the scale must be 'log' or 'linear'");
            return false;
        }
        ++at;
    }
    if (at < nw) {
        fail_key(err, name, "unexpected words after the range");
        return false;
    }
    if (r->log && !(r->lo > 0)) {
        fail_key(err, name, "a log range needs lo above 0: narrow it (range.x = lo hi)");
        return false;
    }
    return true;
}

double from_unit(const SearchRange& r, double u) {
    if (r.log)
        return exp(log(r.lo) + u * (log(r.hi) - log(r.lo)));
    return r.lo + u * (r.hi - r.lo);
}

double to_unit(const SearchRange& r, double v) {
    double u = r.log ? (log(v) - log(r.lo)) / (log(r.hi) - log(r.lo)) : (v - r.lo) / (r.hi - r.lo);
    return u < 0 ? 0 : (u > 1 ? 1 : u);
}

// Four significant digits, read back exactly as the text says.
double round4(double v) {
    if (v == 0)
        return 0;
    char buf[48];
    snprintf(buf, sizeof buf, "%.4g", v);
    return strtod(buf, nullptr);
}

void write_text(const char* path, Str text) {
    write_file(path, text.p, text.n);
}

} // namespace

// --- the search file -----------------------------------------------------------------------

bool search_load(const char* path, SearchSpec* s, TextBuf* err) {
    Config f;
    Config::ParseError pe;
    if (!f.load_file(path, &pe)) {
        err->append(path);
        err->append(": ");
        err->append(pe.msg);
        return false;
    }
    s->path.clear();
    s->path.append(path);
    s->base.clear();
    s->out.clear();
    s->sets.clear();
    s->ranges.clear();
    for (u32 i = 0; i < kScoreCount; ++i) {
        s->weight[i] = 0;
        s->floor[i] = 0;
    }
    s->weight[kScoreLifespan] = 1; // lineage length x novelty ...
    s->weight[kScoreNovelty] = 1;
    s->floor[kScoreGenotypes] = 10;   // ... with a floor on diversity
    s->floor[kScorePersistence] = 0.9; // ... and on population
    for (usize i = 0; i < f.count(); ++i) {
        const Str k = f.key(i), v = f.value(i);
        u64 n = 0;
        double x = 0;
        u32 si = 0;
        auto whole = [&](u32* dst, u64 lo, u64 hi) {
            if (!parse_u64(v, &n) || n < lo || n > hi) {
                char what[96];
                snprintf(what, sizeof what, "must be a whole number from %llu to %llu", static_cast<unsigned long long>(lo),
                         static_cast<unsigned long long>(hi));
                fail_key(err, k, what);
                return false;
            }
            *dst = static_cast<u32>(n);
            return true;
        };
        auto number = [&](double* dst, double lo) {
            if (!parse_f64(v, &x) || x < lo) {
                fail_key(err, k, lo > 0 ? "must be a positive number" : "must be a number, 0 or more");
                return false;
            }
            *dst = x;
            return true;
        };
        bool ok = true;
        if (k.eq("search.base")) {
            s->base.append(v);
        } else if (k.eq("search.out")) {
            s->out.append(v);
        } else if (k.eq("search.seed")) {
            ok = parse_u64(v, &s->seed);
            if (!ok)
                fail_key(err, k, "must be a whole number");
        } else if (k.eq("search.ticks")) {
            ok = parse_u64(v, &s->ticks) && s->ticks > 0;
            if (!ok)
                fail_key(err, k, "must be a whole number above 0");
        } else if (k.eq("search.trials")) {
            ok = whole(&s->trials, 1, 100000);
        } else if (k.eq("search.init")) {
            ok = whole(&s->init, 1, 100000);
        } else if (k.eq("search.seeds")) {
            ok = whole(&s->seeds, 1, 99);
        } else if (k.eq("search.parallel")) {
            ok = whole(&s->parallel, 1, 64);
        } else if (k.eq("search.threads")) {
            ok = whole(&s->threads, 1, 1024);
        } else if (k.eq("search.best")) {
            ok = whole(&s->best, 0, 1000);
        } else if (k.eq("search.body_floor")) {
            ok = number(&s->body_floor, 0);
        } else if (k.eq("search.floor_penalty")) {
            ok = number(&s->floor_penalty, 0);
        } else if (k.eq("search.threshold")) {
            ok = number(&s->threshold, 0);
        } else if (k.starts_with("range.")) {
            SearchRange r;
            ok = parse_range(k.sub(6, k.n - 6), v, &r, err);
            for (usize j = 0; ok && j < s->ranges.size(); ++j)
                if (s->ranges[j].key == r.key) {
                    fail_key(err, k, "is given twice");
                    ok = false;
                }
            if (ok)
                s->ranges.push(r);
        } else if (k.starts_with("set.")) {
            const Str name = k.sub(4, k.n - 4);
            if (!key_find(name)) {
                fail_key(err, k, "is not a setting (nova keys lists them)");
                ok = false;
            } else if (reserved_key(name)) {
                fail_key(err, k, "is the search's own to set");
                ok = false;
            } else {
                s->sets.set(name, v);
            }
        } else if (k.starts_with("score.") || k.starts_with("floor.")) {
            const bool w = k.starts_with("score.");
            if (!score_index(k.sub(6, k.n - 6), &si)) {
                fail_key(err, k, "names no score (lifespan depth genotypes evenness novelty persistence speed)");
                ok = false;
            } else {
                ok = number(w ? &s->weight[si] : &s->floor[si], 0);
            }
        } else {
            fail_key(err, k, "is not a search key (search.*, range.*, set.*, score.*, floor.*)");
            ok = false;
        }
        if (!ok)
            return false;
    }
    if (s->base.size() == 0 || s->out.size() == 0) {
        err->append("the search file needs search.base (the base config) and search.out (the folder)");
        return false;
    }
    if (s->ranges.empty()) {
        err->append("the search file varies nothing: add range.<setting> = lo hi");
        return false;
    }
    for (usize j = 0; j < s->ranges.size(); ++j)
        if (s->sets.has(s->ranges[j].key->name)) {
            fail_key(err, Str(s->ranges[j].key->name), "is both set and searched");
            return false;
        }
    if (s->init > s->trials)
        s->init = s->trials;
    if (!s->base_cfg.load_file(s->base.c_str(), &pe)) {
        err->append(s->base.view());
        err->append(": ");
        err->append(pe.msg);
        return false;
    }
    return true;
}

double search_objective(const SearchSpec& s, const double* scores) {
    double j = 0;
    for (u32 i = 0; i < kScoreCount; ++i) {
        const double v = scores[i] > 0 ? scores[i] : 0.0;
        j += s.weight[i] * log(1 + v);
        if (s.floor[i] > 0 && v < s.floor[i])
            j -= s.floor_penalty * (s.floor[i] - v) / s.floor[i];
    }
    return j;
}

void search_value(const SearchRange& r, double u, TextBuf& text, double* snapped) {
    u = u < 0 ? 0 : (u > 1 ? 1 : u);
    const Key& k = *r.key;
    switch (k.kind) {
    case KeyKind::flag: {
        const bool b = u >= 0.5;
        text.append(b ? "true" : "false");
        *snapped = b ? 1.0 : 0.0;
        return;
    }
    case KeyKind::choice: {
        const u32 n = option_count(k);
        u32 i = static_cast<u32>(u * n);
        if (i >= n)
            i = n - 1;
        text.append(option_at(k, i));
        *snapped = (i + 0.5) / n;
        return;
    }
    case KeyKind::whole: {
        double v = floor(from_unit(r, u) + 0.5);
        v = v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
        if (v < 0)
            text.append_i64(static_cast<i64>(v));
        else
            text.append_u64(static_cast<u64>(v));
        *snapped = to_unit(r, v);
        return;
    }
    default: {
        double v = round4(from_unit(r, u));
        v = v < r.lo ? r.lo : (v > r.hi ? r.hi : v);
        text.append_f64(v);
        *snapped = to_unit(r, v);
        return;
    }
    }
}

void search_trial_config(const SearchSpec& s, const double* u, u64 world_seed, TextBuf& out) {
    Config c;
    TextBuf base;
    s.base_cfg.write(base);
    Config::ParseError pe;
    c.parse(base.view(), &pe);
    // The base's outputs off; the trial's own below, relative to its folder.
    for (usize i = 0; i < c.count(); ++i) {
        const Str k = c.key(i);
        if (k.starts_with("output."))
            c.set(k, k.eq("output.snapshot_every") ? Str("0") : Str(""));
    }
    for (usize i = 0; i < s.sets.count(); ++i)
        c.set(s.sets.key(i), s.sets.value(i));
    for (usize j = 0; j < s.ranges.size(); ++j) {
        TextBuf v;
        double snapped;
        search_value(s.ranges[j], u[j], v, &snapped);
        c.set(s.ranges[j].key->name, v.view());
    }
    TextBuf t;
    t.append_u64(world_seed);
    c.set("seed", t.view());
    t.clear();
    t.append_u64(s.ticks);
    c.set("run.ticks", t.view());
    c.set("run.seconds", "0");
    c.set("run.after_replicator", "-1");
    t.clear();
    t.append_u64(s.threads);
    c.set("run.threads", t.view());
    c.set("output.csv", "stats.csv");
    c.set("output.lineages", "lineages.csv");
    c.set("output.genebank", "genebank.txt");
    c.set("output.result", "result.txt");
    c.write(out);
}

// --- running trials --------------------------------------------------------------------------

namespace {

constexpr u32 kPending = 0, kOk = 1, kFailed = 2;

struct Job {
    u32 trial, seed;
    char dir[MAX_PATH];
};

// Starts `exe run run.cfg --no-esc` in dir, its output to dir\stdout.txt.
HANDLE launch(const char* exe, const char* dir, HANDLE group) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\stdout.txt", dir);
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = in;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    char cmd[MAX_PATH * 2 + 64];
    snprintf(cmd, sizeof cmd, "\"%s\" run run.cfg --no-esc", exe);
    const BOOL ok = CreateProcessA(exe, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir,
                                   &si, &pi);
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE)
        CloseHandle(in);
    if (!ok)
        return nullptr;
    if (group)
        AssignProcessToJobObject(group, pi.hProcess); // dies with the search
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// A finished run of exactly this config is reused (a search started again
// picks up where it stopped).
bool already_done(const char* dir, Str cfg_text) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\run.cfg", dir);
    Array<u8> old;
    if (!read_file(path, &old) || old.size() != cfg_text.n || memcmp(old.data(), cfg_text.p, cfg_text.n) != 0)
        return false;
    snprintf(path, sizeof path, "%s\\result.txt", dir);
    Array<u8> res;
    if (!read_file(path, &res))
        return false;
    res.push(0);
    return strstr(reinterpret_cast<const char*>(res.data()), "stop = ticks") != nullptr;
}

// Runs the jobs, `parallel` at a time; exit[i] gets each one's exit code (-1
// if it couldn't start).
void run_jobs(const char* exe, const Array<Job>& jobs, u32 parallel, HANDLE group, Array<i32>* codes) {
    codes->resize(jobs.size());
    HANDLE live[MAXIMUM_WAIT_OBJECTS];
    usize which[MAXIMUM_WAIT_OBJECTS];
    u32 n = 0;
    usize next = 0;
    if (parallel > MAXIMUM_WAIT_OBJECTS)
        parallel = MAXIMUM_WAIT_OBJECTS;
    while (next < jobs.size() || n > 0) {
        while (n < parallel && next < jobs.size()) {
            HANDLE h = launch(exe, jobs[next].dir, group);
            if (!h) {
                (*codes)[next++] = -1;
                continue;
            }
            live[n] = h;
            which[n] = next++;
            ++n;
        }
        if (n == 0)
            continue;
        const DWORD w = WaitForMultipleObjects(n, live, FALSE, INFINITE);
        if (w >= WAIT_OBJECT_0 + n)
            break; // can't happen; don't spin
        const u32 i = w - WAIT_OBJECT_0;
        DWORD code = 1;
        GetExitCodeProcess(live[i], &code);
        CloseHandle(live[i]);
        (*codes)[which[i]] = static_cast<i32>(code);
        live[i] = live[n - 1];
        which[i] = which[n - 1];
        --n;
    }
}

struct State {
    u32 trials = 0, d = 0;
    Matrix u;      // trials x d, snapped
    Matrix score;  // trials x kScoreCount
    Array<double> obj, pred, psd, ei;
    Array<u32> status, phase; // phase 0: the Latin hypercube, 1: expected improvement
    Array<u8> pareto;
};

// The model's view: every finished trial, a failed one just below the worst.
void observations(const State& st, u32 upto, Matrix* x, Array<double>* y) {
    double worst = 0;
    bool any = false;
    for (u32 t = 0; t < upto; ++t)
        if (st.status[t] == kOk && (!any || st.obj[t] < worst)) {
            worst = st.obj[t];
            any = true;
        }
    u32 n = 0;
    for (u32 t = 0; t < upto; ++t)
        n += st.status[t] != kPending;
    x->resize(n, st.d);
    y->resize(n);
    u32 r = 0;
    for (u32 t = 0; t < upto; ++t) {
        if (st.status[t] == kPending)
            continue;
        memcpy(x->row(r), st.u.row(t), st.d * sizeof(double));
        (*y)[r] = st.status[t] == kOk ? st.obj[t] : worst - 1;
        ++r;
    }
}

// Picks trials [from, to) by expected improvement.
void choose(const SearchSpec& s, State& st, u32 from, u32 to) {
    Matrix x;
    Array<double> y;
    observations(st, from, &x, &y);
    const u32 d = st.d;
    Gp gp;
    gp.fit(x, y.data(), true);
    double best = 0;
    bool any = false;
    for (u32 t = 0; t < from; ++t)
        if (st.status[t] == kOk && (!any || st.obj[t] > best)) {
            best = st.obj[t];
            any = true;
        }
    if (!any && y.size())
        best = y[0];
    // The best five so far (ties to the earlier trial), for local candidates.
    u32 top[5];
    u32 ntop = 0;
    for (; ntop < 5; ++ntop) {
        u32 bi = from;
        for (u32 t = 0; t < from; ++t) {
            bool taken = false;
            for (u32 q = 0; q < ntop; ++q)
                taken = taken || top[q] == t;
            if (!taken && st.status[t] == kOk && (bi == from || st.obj[t] > st.obj[bi]))
                bi = t;
        }
        if (bi == from)
            break;
        top[ntop] = bi;
    }
    for (u32 t = from; t < to; ++t) {
        Rng rng = Rng::from_base(mix64(s.seed ^ 0x6E6F76612D736561ull) + t); // "nova-sea(rch)"
        const u32 nuni = 1000 + 250 * d > 6000 ? 6000 : 1000 + 250 * d;
        const u32 nloc = 100 * ntop;
        Matrix cand;
        cand.resize(nuni + nloc, d);
        for (u32 c = 0; c < nuni; ++c)
            for (u32 j = 0; j < d; ++j)
                cand.at(c, j) = rng.unit();
        for (u32 c = 0; c < nloc; ++c) {
            const double* base = st.u.row(top[c / 100]);
            for (u32 j = 0; j < d; ++j) {
                const double u1 = 1.0 - rng.unit(), u2 = rng.unit();
                const double z = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
                double v = base[j] + 0.05 * z;
                cand.at(nuni + c, j) = v < 0 ? 0 : (v > 1 ? 1 : v);
            }
        }
        Array<double> e;
        e.resize(cand.rows);
        for (usize c = 0; c < cand.rows; ++c)
            e[c] = gp.expected_improvement(cand.row(c), best, 0.01 * gp.signal_sd());
        Array<double> snap;
        snap.resize(d);
        bool placed = false;
        for (u32 tries = 0; tries < 200 && !placed; ++tries) {
            usize bi = cand.rows;
            for (usize c = 0; c < cand.rows; ++c)
                if (e[c] >= 0 && (bi == cand.rows || e[c] > e[bi]))
                    bi = c;
            if (bi == cand.rows)
                break;
            for (u32 j = 0; j < d; ++j) {
                TextBuf tmp;
                search_value(s.ranges[j], cand.at(bi, j), tmp, &snap[j]);
            }
            bool dup = false;
            for (u32 q = 0; q < t && !dup; ++q)
                dup = memcmp(st.u.row(q), snap.data(), d * sizeof(double)) == 0;
            if (dup) {
                e[bi] = -1;
                continue;
            }
            placed = true;
        }
        if (!placed) // every candidate repeats a trial: take a fresh random point
            for (u32 j = 0; j < d; ++j) {
                TextBuf tmp;
                search_value(s.ranges[j], rng.unit(), tmp, &snap[j]);
            }
        memcpy(st.u.row(t), snap.data(), d * sizeof(double));
        gp.predict(snap.data(), &st.pred[t], &st.psd[t]);
        st.ei[t] = gp.expected_improvement(snap.data(), best, 0.0);
        st.phase[t] = 1;
        // The next pick believes this one comes out as predicted.
        Matrix x2;
        x2.resize(x.rows + 1, d);
        memcpy(x2.v.data(), x.v.data(), x.v.size() * sizeof(double));
        memcpy(x2.row(x.rows), snap.data(), d * sizeof(double));
        y.push(st.pred[t]);
        x = nova::move(x2);
        gp.fit(x, y.data(), false);
    }
}

void append_num(TextBuf& out, double v, const char* fmt = "%.6g") {
    char num[48];
    snprintf(num, sizeof num, fmt, v);
    out.append(num);
}

bool in_pareto(const SearchSpec& s, u32 i) {
    return i != kScoreSpeed || s.weight[kScoreSpeed] != 0;
}

void mark_pareto(const SearchSpec& s, State& st) {
    st.pareto.resize(st.trials);
    st.pareto.fill(0);
    for (u32 a = 0; a < st.trials; ++a) {
        if (st.status[a] != kOk)
            continue;
        bool beaten = false;
        for (u32 b = 0; b < st.trials && !beaten; ++b) {
            if (b == a || st.status[b] != kOk)
                continue;
            bool ge = true, gt = false;
            for (u32 i = 0; i < kScoreCount; ++i) {
                if (!in_pareto(s, i))
                    continue;
                const double va = st.score.at(a, i), vb = st.score.at(b, i);
                ge = ge && vb >= va;
                gt = gt || vb > va;
            }
            beaten = ge && gt;
        }
        st.pareto[a] = beaten ? 0 : 1;
    }
}

void write_summary(const SearchSpec& s, const State& st, const char* path) {
    TextBuf t;
    t.append("trial,phase,status");
    for (usize j = 0; j < s.ranges.size(); ++j) {
        t.append_char(',');
        t.append(s.ranges[j].key->name);
    }
    for (u32 i = 0; i < kScoreCount; ++i) {
        t.append_char(',');
        t.append(score_name(i));
    }
    t.append(",objective,predicted,predicted_sd,expected_improvement,pareto\n");
    for (u32 tr = 0; tr < st.trials; ++tr) {
        t.append_u64(tr + 1);
        t.append(st.phase[tr] ? ",ei," : ",lhs,");
        t.append(st.status[tr] == kOk ? "ok" : (st.status[tr] == kFailed ? "failed" : "pending"));
        for (usize j = 0; j < s.ranges.size(); ++j) {
            t.append_char(',');
            double snapped;
            search_value(s.ranges[j], st.u.at(tr, j), t, &snapped);
        }
        for (u32 i = 0; i < kScoreCount; ++i) {
            t.append_char(',');
            if (st.status[tr] == kOk)
                append_num(t, st.score.at(tr, i));
        }
        t.append_char(',');
        if (st.status[tr] == kOk)
            append_num(t, st.obj[tr]);
        t.append_char(',');
        if (st.phase[tr]) {
            append_num(t, st.pred[tr]);
            t.append_char(',');
            append_num(t, st.psd[tr]);
            t.append_char(',');
            append_num(t, st.ei[tr]);
        } else {
            t.append(",,");
        }
        t.append_char(',');
        t.append(st.pareto.size() > tr && st.pareto[tr] ? "1" : "0");
        t.append_char('\n');
    }
    write_text(path, t.view());
}

void trial_dir(const char* root, u32 trial, u32 seed, char* out, usize cap) {
    snprintf(out, cap, "%s\\trial_%03u\\seed_%u", root, trial + 1, seed + 1);
}

u64 world_seed(const SearchSpec& s, u32 k) {
    return s.seed * 1000 + k + 1;
}

} // namespace

bool search_run(SearchSpec& s, const SearchOptions& o, TextBuf* err) {
    const u32 d = static_cast<u32>(s.ranges.size());
    State st;
    st.trials = s.trials;
    st.d = d;
    st.u.resize(s.trials, d);
    st.score.resize(s.trials, kScoreCount);
    st.obj.resize(s.trials);
    st.pred.resize(s.trials);
    st.psd.resize(s.trials);
    st.ei.resize(s.trials);
    st.status.resize(s.trials);
    st.phase.resize(s.trials);

    // The output folder as an absolute path (the runs start in their own folders).
    char full[MAX_PATH];
    const DWORD fl = GetFullPathNameA(s.out.c_str(), sizeof full, full, nullptr);
    if (fl == 0 || fl >= sizeof full) {
        err->append("search.out: a path that's too long");
        return false;
    }
    s.out.clear();
    s.out.append(full);
    const u32 init = s.init < s.trials ? s.init : s.trials;
    Matrix lhs;
    latin_hypercube(init, d, s.seed, &lhs);
    for (u32 t = 0; t < init; ++t)
        for (u32 j = 0; j < d; ++j) {
            TextBuf tmp;
            search_value(s.ranges[j], lhs.at(t, j), tmp, &st.u.at(t, j));
        }

    if (!o.quiet) {
        printf("nova search: %u settings, %u trials (%u spread first), %u seed%s each, %u runs at once of %u "
               "thread%s, %llu ticks a run\n",
               d, s.trials, init, s.seeds, s.seeds == 1 ? "" : "s", s.parallel, s.threads, s.threads == 1 ? "" : "s",
               static_cast<unsigned long long>(s.ticks));
        for (u32 j = 0; j < d; ++j) {
            const SearchRange& r = s.ranges[j];
            printf("  %-28s %s %g to %g%s\n", r.key->name, key_kind_name(r.key->kind), r.lo, r.hi,
                   r.log ? " (log scale)" : "");
        }
        printf("  objective:");
        bool first = true;
        for (u32 i = 0; i < kScoreCount; ++i)
            if (s.weight[i] != 0) {
                printf("%s %g x ln(1 + %s)", first ? "" : " +", s.weight[i], score_name(i));
                first = false;
            }
        for (u32 i = 0; i < kScoreCount; ++i)
            if (s.floor[i] > 0)
                printf(", %s at least %g", score_name(i), s.floor[i]);
        printf("\n");
        if (s.weight[kScoreSpeed] != 0)
            printf("nova search: score.speed is weighted, and speed is measured by the clock: this search will not "
                   "repeat exactly\n");
        fflush(stdout);
    }
    if (o.dry_run) {
        if (!o.quiet) {
            printf("the first %u trials (a Latin hypercube from search.seed %llu):\n", init,
                   static_cast<unsigned long long>(s.seed));
            for (u32 t = 0; t < init; ++t) {
                TextBuf line;
                for (u32 j = 0; j < d; ++j) {
                    line.append(j ? ", " : "  ");
                    line.append(s.ranges[j].key->name);
                    line.append(" = ");
                    double snapped;
                    search_value(s.ranges[j], st.u.at(t, j), line, &snapped);
                }
                printf("  %3u:%s\n", t + 1, line.c_str());
            }
        }
        return true;
    }
    if (!o.nova_exe || !path_exists(o.nova_exe)) {
        err->append("the nova program to run the trials with wasn't found");
        return false;
    }
    if (!make_dirs(s.out.c_str())) {
        err->append("cannot create '");
        err->append(s.out.view());
        err->append_char('\'');
        return false;
    }
    {
        Array<u8> text;
        TextBuf p;
        path_join(p, s.out.view(), "search.cfg");
        if (read_file(s.path.c_str(), &text))
            write_file(p.c_str(), text.data(), text.size());
    }
    // Every run joins a job object, so stopping the search stops its runs.
    HANDLE group = CreateJobObjectA(nullptr, nullptr);
    if (group) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(group, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    TextBuf summary;
    path_join(summary, s.out.view(), "summary.csv");
    ScoreOptions so;
    so.novelty_threshold = s.threshold;
    const u32 batch = s.parallel / s.seeds ? s.parallel / s.seeds : 1;
    u32 next = 0;
    while (next < s.trials) {
        const u32 end = next < init ? init : (next + batch < s.trials ? next + batch : s.trials);
        if (next >= init)
            choose(s, st, next, end);
        Array<Job> jobs;
        Array<u8> reused;
        for (u32 t = next; t < end; ++t) {
            for (u32 k = 0; k < s.seeds; ++k) {
                Job j;
                j.trial = t;
                j.seed = k;
                trial_dir(full, t, k, j.dir, sizeof j.dir);
                make_dirs(j.dir);
                TextBuf cfg;
                cfg.append("# nova search: trial ");
                cfg.append_u64(t + 1);
                cfg.append(", seed ");
                cfg.append_u64(k + 1);
                cfg.append(" of ");
                cfg.append_u64(s.seeds);
                cfg.append(" (");
                cfg.append(s.path.view());
                cfg.append(").\n# Repeat it from this folder: nova run run.cfg\n");
                search_trial_config(s, st.u.row(t), world_seed(s, k), cfg);
                if (already_done(j.dir, cfg.view())) {
                    reused.push(1);
                } else {
                    char path[MAX_PATH + 32];
                    snprintf(path, sizeof path, "%s\\run.cfg", j.dir);
                    write_text(path, cfg.view());
                    reused.push(0);
                }
                jobs.push(j);
            }
        }
        Array<Job> todo;
        for (usize i = 0; i < jobs.size(); ++i)
            if (!reused[i])
                todo.push(jobs[i]);
        Array<i32> code;
        run_jobs(o.nova_exe, todo, s.parallel, group, &code);
        Array<i32> exit_of;
        exit_of.resize(jobs.size());
        for (usize i = 0, k = 0; i < jobs.size(); ++i)
            exit_of[i] = reused[i] ? 0 : code[k++];
        for (u32 t = next; t < end; ++t) {
            double sum[kScoreCount] = {};
            bool ok = true;
            for (u32 k = 0; k < s.seeds; ++k) {
                const usize ji = (t - next) * s.seeds + k;
                if (exit_of[ji] != 0) {
                    ok = false;
                    continue;
                }
                Config c;
                TextBuf cerr;
                so.body_floor = 0;
                if (load_run_config(jobs[ji].dir, &c, &cerr))
                    so.body_floor = s.body_floor * static_cast<double>(key_peek_u64(c, "world.nodes"));
                if (so.body_floor <= 0)
                    so.body_floor = s.body_floor;
                RunScores rs;
                TextBuf serr;
                if (!score_run(jobs[ji].dir, so, &rs, &serr) || rs.ticks != s.ticks) {
                    ok = false;
                    continue;
                }
                for (u32 i = 0; i < kScoreCount; ++i)
                    sum[i] += rs.s[i];
            }
            st.status[t] = ok ? kOk : kFailed;
            if (ok) {
                for (u32 i = 0; i < kScoreCount; ++i)
                    st.score.at(t, i) = sum[i] / s.seeds;
                st.obj[t] = search_objective(s, st.score.row(t));
            }
            if (!o.quiet) {
                TextBuf line;
                for (u32 j = 0; j < d; ++j) {
                    line.append(j ? ", " : "");
                    line.append(s.ranges[j].key->name);
                    line.append(" = ");
                    double snapped;
                    search_value(s.ranges[j], st.u.at(t, j), line, &snapped);
                }
                if (ok)
                    printf("trial %u/%u (%s): objective %.4g  lifespan %.0f, novelty %.3g, genotypes %.0f, "
                           "persistence %.2f, %.0f ticks/s  [%s]\n",
                           t + 1, s.trials, st.phase[t] ? "ei" : "lhs", st.obj[t], st.score.at(t, kScoreLifespan),
                           st.score.at(t, kScoreNovelty), st.score.at(t, kScoreGenotypes),
                           st.score.at(t, kScorePersistence), st.score.at(t, kScoreSpeed), line.c_str());
                else
                    printf("trial %u/%u (%s): FAILED (see its stdout.txt)  [%s]\n", t + 1, s.trials,
                           st.phase[t] ? "ei" : "lhs", line.c_str());
                fflush(stdout);
            }
        }
        mark_pareto(s, st);
        write_summary(s, st, summary.c_str());
        // Nothing has run at all: the base config, a set.* or a range is
        // wrong for every trial (a key the world never reads, say), so stop
        // and show why rather than spend the search on it.
        u32 ok_so_far = 0;
        for (u32 t = 0; t < end; ++t)
            ok_so_far += st.status[t] == kOk;
        if (ok_so_far == 0) {
            char p[MAX_PATH + 32];
            snprintf(p, sizeof p, "%s\\stdout.txt", jobs[0].dir);
            Array<u8> text;
            read_file(p, &text);
            usize from = text.size() > 400 ? text.size() - 400 : 0;
            err->append("every trial so far failed; the first run's output (");
            err->append(p);
            err->append(") ends:\n");
            err->append(Str(reinterpret_cast<const char*>(text.data()) + from, text.size() - from));
            if (group)
                CloseHandle(group);
            return false;
        }
        next = end;
    }
    if (group)
        CloseHandle(group);

    // The best configurations, loadable as they are.
    Array<u8> used;
    used.resize(s.trials);
    TextBuf names;
    for (u32 b = 0; b < s.best; ++b) {
        u32 bi = s.trials;
        for (u32 t = 0; t < s.trials; ++t)
            if (st.status[t] == kOk && !used[t] && (bi == s.trials || st.obj[t] > st.obj[bi]))
                bi = t;
        if (bi == s.trials)
            break;
        used[bi] = 1;
        Config c;
        TextBuf base;
        s.base_cfg.write(base);
        Config::ParseError pe;
        c.parse(base.view(), &pe);
        for (usize i = 0; i < s.sets.count(); ++i)
            c.set(s.sets.key(i), s.sets.value(i));
        for (u32 j = 0; j < d; ++j) {
            TextBuf v;
            double snapped;
            search_value(s.ranges[j], st.u.at(bi, j), v, &snapped);
            c.set(s.ranges[j].key->name, v.view());
        }
        TextBuf t;
        t.append("# nova search: best ");
        t.append_u64(b + 1);
        t.append(" of ");
        t.append(s.path.view());
        t.append(": trial ");
        t.append_u64(bi + 1);
        t.append(", objective ");
        append_num(t, st.obj[bi]);
        t.append("\n#");
        for (u32 i = 0; i < kScoreCount; ++i) {
            t.append_char(' ');
            t.append(score_name(i));
            t.append_char(' ');
            append_num(t, st.score.at(bi, i), "%.4g");
        }
        t.append("\n# (the base config with the search's settings; its seed and outputs are the base's)\n");
        c.write(t);
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%s\\best_%u.cfg", full, b + 1);
        write_text(p, t.view());
    }

    // The settings by influence: short fitted length scales matter most.
    Matrix x;
    Array<double> y;
    observations(st, s.trials, &x, &y);
    if (x.rows >= 3) {
        Gp gp;
        gp.fit(x, y.data(), true);
        double total = 0;
        for (u32 j = 0; j < d; ++j)
            total += 1.0 / gp.length_scale(j);
        Array<u8> done;
        done.resize(d);
        TextBuf t;
        t.append("rank,setting,length_scale,influence\n");
        if (!o.quiet)
            printf("nova search: settings by influence (from the fitted length scales):\n");
        for (u32 r = 0; r < d; ++r) {
            u32 bj = d;
            for (u32 j = 0; j < d; ++j)
                if (!done[j] && (bj == d || gp.length_scale(j) < gp.length_scale(bj)))
                    bj = j;
            done[bj] = 1;
            const double inf = (1.0 / gp.length_scale(bj)) / total;
            t.append_u64(r + 1);
            t.append_char(',');
            t.append(s.ranges[bj].key->name);
            t.append_char(',');
            append_num(t, gp.length_scale(bj), "%.4g");
            t.append_char(',');
            append_num(t, inf, "%.4f");
            t.append_char('\n');
            if (!o.quiet)
                printf("  %u. %-28s length scale %.3g, influence %.1f%%\n", r + 1, s.ranges[bj].key->name,
                       gp.length_scale(bj), 100 * inf);
        }
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%s\\influence.csv", full);
        write_text(p, t.view());
    }

    // The best trade-offs.
    {
        TextBuf t;
        t.append("trial,objective");
        for (u32 i = 0; i < kScoreCount; ++i) {
            t.append_char(',');
            t.append(score_name(i));
        }
        t.append_char('\n');
        u32 front = 0;
        for (u32 tr = 0; tr < s.trials; ++tr) {
            if (st.pareto.size() <= tr || !st.pareto[tr])
                continue;
            ++front;
            t.append_u64(tr + 1);
            t.append_char(',');
            append_num(t, st.obj[tr]);
            for (u32 i = 0; i < kScoreCount; ++i) {
                t.append_char(',');
                append_num(t, st.score.at(tr, i));
            }
            t.append_char('\n');
        }
        char p[MAX_PATH + 32];
        snprintf(p, sizeof p, "%s\\pareto.csv", full);
        write_text(p, t.view());
        u32 ok = 0, bi = s.trials;
        for (u32 tr = 0; tr < s.trials; ++tr)
            if (st.status[tr] == kOk) {
                ++ok;
                if (bi == s.trials || st.obj[tr] > st.obj[bi])
                    bi = tr;
            }
        if (!o.quiet) {
            printf("nova search: %u of %u trials ran; %u on the best trade-offs (pareto.csv)", ok, s.trials, front);
            if (bi < s.trials)
                printf("; the best is trial %u, objective %.4g (best_1.cfg)", bi + 1, st.obj[bi]);
            printf("\nnova search: wrote %s\n", full);
        }
    }
    return true;
}

} // namespace nova
