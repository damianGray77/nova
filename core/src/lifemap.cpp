// The map of life (docs/analysis-plan.md A5; docs/results/viewer.md): every
// genotype as a point, its hashed 3-codon runs reduced to two dimensions by
// PCA (learn/pca.cpp) fitted once on a reference window, so positions keep
// their meaning over time. Plain code with no Win32 in it: the viewer draws
// it, the tests drive it.
#include <cmath> // sqrt

#include "nova/hash.h"
#include "nova/view.h"

namespace nova {

namespace {

constexpr u32 kMaxRuns = 4096; // 3-codon runs a genome contributes at most (genomes are far shorter)

u64 token_hash(const char* p, usize n) { return hash_bytes(p, n, 0x6C6966652D6F7073ull); } // "life-ops"

// A genome's 3-codon runs as buckets (one per run, in order; repeats kept).
// Returns how many runs.
u32 runs_of(Str ops, u16* bucket, u32 cap) {
    u64 h[3] = {};
    u32 seen = 0, n = 0;
    usize i = 0;
    while (i < ops.n && n < cap) {
        while (i < ops.n && ops.p[i] == ' ')
            ++i;
        usize j = i;
        while (j < ops.n && ops.p[j] != ' ')
            ++j;
        if (j == i)
            break;
        h[0] = h[1];
        h[1] = h[2];
        h[2] = token_hash(ops.p + i, j - i);
        ++seen;
        if (seen >= 3)
            bucket[n++] = static_cast<u16>(mix64(h[0] ^ mix64(h[1] ^ mix64(h[2] + kGolden))) & (kLifeDims - 1));
        i = j;
    }
    return n;
}

// The sparse features: distinct buckets, increasing, with sqrt(count / runs).
u32 sparse_features(Str ops, u16* bucket, double* value) {
    u16 runs[kMaxRuns];
    const u32 n = runs_of(ops, runs, kMaxRuns);
    if (n == 0)
        return 0;
    // Counting sort over the buckets seen (n is small): insertion sort, then merge equal ones.
    for (u32 a = 1; a < n; ++a) {
        const u16 v = runs[a];
        u32 b = a;
        while (b > 0 && runs[b - 1] > v) {
            runs[b] = runs[b - 1];
            --b;
        }
        runs[b] = v;
    }
    u32 m = 0;
    for (u32 a = 0; a < n;) {
        u32 b = a;
        while (b < n && runs[b] == runs[a])
            ++b;
        bucket[m] = runs[a];
        value[m] = sqrt(static_cast<double>(b - a) / n);
        ++m;
        a = b;
    }
    return m;
}

// Doubles in order through their bit patterns (negatives flipped).
u64 order_key(double v) {
    u64 b;
    memcpy(&b, &v, 8);
    return (b >> 63) ? ~b : (b | 0x8000000000000000ull);
}
double from_key(u64 k) {
    const u64 b = (k >> 63) ? (k & 0x7FFFFFFFFFFFFFFFull) : ~k;
    double v;
    memcpy(&v, &b, 8);
    return v;
}

} // namespace

bool life_features(Str ops, double* dense) {
    for (u32 j = 0; j < kLifeDims; ++j)
        dense[j] = 0;
    u16 bucket[kMaxRuns];
    double value[kMaxRuns];
    const u32 m = sparse_features(ops, bucket, value);
    for (u32 k = 0; k < m; ++k)
        dense[bucket[k]] = value[k];
    return m > 0;
}

void LifeMap::clear() {
    pts_.clear();
    index_.clear();
    saves_ = 0;
    t0_ = t1_ = 0;
    fitted_ = final_ = false;
    ref_n_ = 0;
    next_fit_ = 8;
    ref_first_ = ref_last_ = 0;
    mean_.clear();
    var_.clear();
    comps_.resize(0, 0);
    mu_dot_[0] = mu_dot_[1] = 0;
    total_var_ = 0;
    box_[0] = box_[1] = -1;
    box_[2] = box_[3] = 1;
    held_text_.clear();
    held_off_.clear();
    held_len_.clear();
}

u32 LifeMap::find(u64 genotype) const {
    const u32* i = index_.find(genotype);
    return i ? *i : kNone;
}

double LifeMap::variance_share(u32 axis) const {
    return axis < var_.size() && total_var_ > 0 ? var_[axis] / total_var_ : 0.0;
}

void LifeMap::place(Str ops, float* x, float* y) const {
    *x = *y = 0;
    if (!fitted_ || comps_.rows < 2)
        return;
    u16 bucket[kMaxRuns];
    double value[kMaxRuns];
    const u32 m = sparse_features(ops, bucket, value);
    if (m == 0)
        return;
    // (r - mean) . axis, with r sparse: r . axis - mean . axis.
    double out[2];
    for (u32 c = 0; c < 2; ++c) {
        const double* u = comps_.row(c);
        double s = 0;
        for (u32 k = 0; k < m; ++k)
            s += value[k] * u[bucket[k]];
        out[c] = s - mu_dot_[c];
    }
    *x = static_cast<float>(out[0]);
    *y = static_cast<float>(out[1]);
}

bool LifeMap::fit_on(const Array<char>& text, const Array<u32>& off, const Array<u32>& len) {
    const usize n = off.size();
    if (n < 2)
        return false;
    Matrix x;
    x.resize(n, kLifeDims);
    for (usize i = 0; i < n; ++i)
        life_features(Str(text.data() + off[i], len[i]), x.row(i));
    // Power iteration (pca.cpp): fixed starts and signs, so the same reference
    // gives the same axes bit for bit.
    pca(x, 2, &mean_, &comps_, &var_, 300, 1e-10);
    for (u32 c = 0; c < 2; ++c)
        mu_dot_[c] = c < comps_.rows ? dot(mean_.data(), comps_.row(c), kLifeDims) : 0.0;
    total_var_ = 0;
    for (usize i = 0; i < n; ++i) {
        const double* r = x.row(i);
        for (u32 j = 0; j < kLifeDims; ++j)
            total_var_ += (r[j] - mean_[j]) * (r[j] - mean_[j]);
    }
    total_var_ = n > 1 ? total_var_ / static_cast<double>(n - 1) : 0.0;
    fitted_ = true;
    ref_n_ = static_cast<u32>(n);
    // The box: the reference's 2nd to 98th percentile on each axis, widened.
    Array<u64> kx, ky;
    kx.resize(n);
    ky.resize(n);
    for (usize i = 0; i < n; ++i) {
        float px = 0, py = 0;
        place(Str(text.data() + off[i], len[i]), &px, &py);
        kx[i] = order_key(px);
        ky[i] = order_key(py);
    }
    sort_u64(kx.data(), n);
    sort_u64(ky.data(), n);
    const usize lo = (n - 1) * 2 / 100, hi = (n - 1) * 98 / 100;
    double x0 = from_key(kx[lo]), x1 = from_key(kx[hi]), y0 = from_key(ky[lo]), y1 = from_key(ky[hi]);
    const double padx = (x1 - x0) * 0.15 + 1e-3, pady = (y1 - y0) * 0.15 + 1e-3;
    box_[0] = static_cast<float>(x0 - padx);
    box_[1] = static_cast<float>(y0 - pady);
    box_[2] = static_cast<float>(x1 + padx);
    box_[3] = static_cast<float>(y1 + pady);
    return true;
}

void LifeMap::place_all() {
    for (usize i = 0; i < pts_.size() && i < held_off_.size(); ++i)
        place(Str(held_text_.data() + held_off_[i], held_len_[i]), &pts_[i].x, &pts_[i].y);
}

bool LifeMap::fit(const Array<char>& text, const Array<u32>& off, const Array<u32>& len, u64 first, u64 last) {
    if (!fit_on(text, off, len))
        return false;
    final_ = true;
    ref_first_ = first;
    ref_last_ = last;
    place_all(); // (the points it holds genomes for: none, when fit comes first)
    held_text_.clear();
    held_off_.clear();
    held_len_.clear();
    return true;
}

void LifeMap::finalize() {
    if (final_)
        return;
    if (pts_.size() >= 2 && fit_on(held_text_, held_off_, held_len_)) {
        place_all();
        ref_first_ = pts_[0].first;
        ref_last_ = pts_.back().first;
    }
    final_ = fitted_;
    if (final_) {
        held_text_.clear();
        held_off_.clear();
        held_len_.clear();
    }
}

void LifeMap::add(u64 tick, u64 genotype, u64 lineage, u32 node, u32 len, const char* name, Str ops) {
    if (saves_ == 0 || tick < t0_)
        t0_ = tick;
    if (tick > t1_)
        t1_ = tick;
    ++saves_;
    bool fresh = false;
    u32& at = index_.get_or_insert(genotype, &fresh);
    if (!fresh) {
        LifePoint& p = pts_[at];
        ++p.saves;
        p.last = tick > p.last ? tick : p.last;
        return;
    }
    at = static_cast<u32>(pts_.size());
    LifePoint p;
    memset(&p, 0, sizeof p);
    p.genotype = genotype;
    p.lineage = lineage;
    p.first = p.last = tick;
    p.saves = 1;
    p.node = node;
    p.len = len;
    if (name) {
        usize k = 0;
        for (; k < sizeof p.name - 1 && name[k]; ++k)
            p.name[k] = name[k];
        p.name[k] = '\0';
    }
    place(ops, &p.x, &p.y);
    pts_.push(p);
    if (final_)
        return;
    // The reference window: the first kLifeRef genotypes, held until the axes
    // are final; provisional fits on the way, each placing every point again.
    held_off_.push(static_cast<u32>(held_text_.size()));
    held_len_.push(static_cast<u32>(ops.n));
    held_text_.append(ops.p, ops.n);
    const usize n = pts_.size();
    if (n >= ref_target_) {
        if (fit_on(held_text_, held_off_, held_len_)) {
            place_all();
            final_ = true;
            ref_first_ = pts_[0].first;
            ref_last_ = pts_.back().first;
            held_text_.clear();
            held_off_.clear();
            held_len_.clear();
        }
    } else if (n >= next_fit_) {
        if (fit_on(held_text_, held_off_, held_len_)) {
            place_all();
            ref_first_ = pts_[0].first;
            ref_last_ = pts_.back().first;
        }
        next_fit_ *= 2;
    }
}

} // namespace nova
