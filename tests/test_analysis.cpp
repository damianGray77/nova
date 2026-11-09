// Suite `analysis`: learning from the runs (docs/analysis-plan.md). The
// hand-rolled models on planted data (A0), the novelty meter against
// hand-worked values (A2), alignment and neighbour joining (A4), the Gaussian
// process and expected improvement, run files read by name, and a tiny real
// search that repeats exactly, whose trial configs reproduce their hashes (A1).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/analysis.h"
#include "nova/io.h"
#include "nova/learn.h"
#include "nova/rng.h"
#include "nova/run.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

bool close_to(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

double gauss(Rng& r) {
    const double u1 = 1.0 - r.unit(), u2 = r.unit();
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

void put(const char* path, const char* text) {
    write_file(path, text, strlen(text));
}

// Removes a folder and everything in it.
void remove_tree(const char* dir) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char p[MAX_PATH];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(p);
            else
                DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

// --- A0: matrices, PCA, k-means -----------------------------------------------------------

void test_cholesky() {
    const double a[9] = {4, 2, 0.6, 2, 5, 1, 0.6, 1, 3};
    Matrix m;
    m.resize(3, 3);
    memcpy(m.v.data(), a, sizeof a);
    CHECK(cholesky(m));
    bool same = true; // L L^T = A
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k)
                s += m.at(i, k) * m.at(j, k);
            same = same && close_to(s, a[i * 3 + j], 1e-12);
        }
    CHECK(same);
    CHECK(m.at(0, 1) == 0 && m.at(0, 2) == 0 && m.at(1, 2) == 0);
    const double x[3] = {1, -2, 0.5};
    double b[3];
    for (int i = 0; i < 3; ++i)
        b[i] = a[i * 3] * x[0] + a[i * 3 + 1] * x[1] + a[i * 3 + 2] * x[2];
    cholesky_solve(m, b);
    CHECK(close_to(b[0], 1, 1e-12) && close_to(b[1], -2, 1e-12) && close_to(b[2], 0.5, 1e-12));
    // det A by cofactors
    const double det = 4 * (5 * 3 - 1 * 1) - 2 * (2 * 3 - 1 * 0.6) + 0.6 * (2 * 1 - 5 * 0.6);
    CHECK(close_to(cholesky_logdet(m), log(det), 1e-12));
    Matrix bad; // not positive definite
    bad.resize(2, 2);
    bad.at(0, 0) = 1;
    bad.at(0, 1) = bad.at(1, 0) = 2;
    bad.at(1, 1) = 1;
    CHECK(!cholesky(bad));
}

void test_pca() {
    // 400 points in 5 dimensions: spread 3 along u1, 1 along u2, a little noise.
    const double r2 = 0.7071067811865476;
    const double u1[5] = {r2, r2, 0, 0, 0}, u2[5] = {0, 0, r2, -r2, 0};
    Rng rng = Rng::from_base(11);
    Matrix x;
    x.resize(400, 5);
    for (usize i = 0; i < 400; ++i) {
        const double a = 3 * gauss(rng), b = gauss(rng);
        for (usize j = 0; j < 5; ++j)
            x.at(i, j) = 2.0 + a * u1[j] + b * u2[j] + 0.05 * gauss(rng);
    }
    Array<double> mean, var;
    Matrix comps;
    pca(x, 3, &mean, &comps, &var);
    CHECK(fabs(dot(comps.row(0), u1, 5)) > 0.999);
    CHECK(fabs(dot(comps.row(1), u2, 5)) > 0.999);
    CHECK(close_to(var[0], 9, 1.5) && close_to(var[1], 1, 0.25) && var[2] < 0.01);
    CHECK(close_to(mean[0], 2, 0.3) && close_to(mean[4], 2, 0.02));
    double p[3];
    pca_project(x.row(0), mean, comps, p);
    CHECK(fabs(p[2]) < 0.3);
    // The same data gives the same components, bit for bit.
    Array<double> mean2, var2;
    Matrix comps2;
    pca(x, 3, &mean2, &comps2, &var2);
    CHECK(memcmp(comps.v.data(), comps2.v.data(), comps.v.size() * sizeof(double)) == 0);
    printf("  pca: variances %.3f %.3f %.5f along the planted axes\n", var[0], var[1], var[2]);
}

void test_kmeans() {
    const double cx[3] = {0, 10, 0}, cy[3] = {0, 0, 10};
    Rng rng = Rng::from_base(5);
    Matrix x;
    x.resize(150, 2);
    for (usize i = 0; i < 150; ++i) {
        x.at(i, 0) = cx[i / 50] + 0.5 * gauss(rng);
        x.at(i, 1) = cy[i / 50] + 0.5 * gauss(rng);
    }
    Matrix c;
    Array<u32> lab;
    const double inertia = kmeans(x, 3, 99, 100, &c, &lab);
    bool blobs = true;
    for (usize i = 0; i < 150; ++i)
        blobs = blobs && lab[i] == lab[(i / 50) * 50];
    CHECK(blobs);
    CHECK(lab[0] != lab[50] && lab[0] != lab[100] && lab[50] != lab[100]);
    CHECK(inertia < 150 * 2 * 0.25 * 1.6);
    Matrix c2;
    Array<u32> lab2;
    CHECK(kmeans(x, 3, 99, 100, &c2, &lab2) == inertia);
}

// --- A2: n-grams and the novelty meter -------------------------------------------------------

void test_ngram_hand() {
    NgramModel m;
    CHECK(m.init(4, 4));
    const u8 s[4] = {0, 1, 2, 3};
    CHECK(close_to(m.bits(s, 4), 8.0, 1e-12)); // empty: 2 bits a symbol
    m.learn(s, 4);
    // Witten-Bell by hand after one "0 1 2 3":
    //   P(0)         = (1 + 4 x 1/4) / (4 + 4)        = 0.25
    //   P(1 | 0)     = (1 + 1 x 0.25) / (1 + 1)       = 0.625
    //   P(2 | 0 1)   = (1 + 1 x P(2 | 1) = 0.625) / 2 = 0.8125
    //   P(3 | 0 1 2) = (1 + 1 x P(3 | 1 2) = 0.8125) / 2 = 0.90625
    //   P(2 | 0)     = (0 + 1 x 0.25) / (1 + 1)       = 0.125
    CHECK(close_to(m.prob(s, 0, 0), 0.25, 1e-15));
    CHECK(close_to(m.prob(s, 1, 1), 0.625, 1e-15));
    CHECK(close_to(m.prob(s, 2, 2), 0.8125, 1e-15));
    CHECK(close_to(m.prob(s, 3, 3), 0.90625, 1e-15));
    CHECK(close_to(m.prob(s, 1, 2), 0.125, 1e-15));
    const double want = 2 - log2(0.625) - log2(0.8125) - log2(0.90625); // 3.11966...
    CHECK(close_to(m.bits(s, 4), want, 1e-12));
    CHECK(close_to(m.surprise(s, 4), want / 4, 1e-12));
    // Only the last three symbols are context at order 4.
    const u8 ctx[5] = {3, 3, 0, 1, 2};
    CHECK(close_to(m.prob(ctx, 5, 3), 0.90625, 1e-15));
    CHECK(!m.init(1, 4) && !m.init(65, 4) && !m.init(4, 5));
}

// A genome of n symbols from 49, and a mutant with k substitutions.
void random_genome(Rng& r, u8* g, u32 n) {
    for (u32 i = 0; i < n; ++i)
        g[i] = static_cast<u8>(r.below(49));
}

void test_novelty_planted() {
    Rng rng = Rng::from_base(77);
    u8 base[3][30];
    for (auto& b : base)
        random_genome(rng, b, 30);
    NoveltyOptions o;
    o.window = 100;
    o.threshold = 3.0;
    NoveltyMeter m;
    CHECK(m.init(49, o));
    u8 g[30];
    double late_mutant = 0;
    u64 tick = 0;
    for (u32 i = 0; i < 300; ++i, tick += 2) {
        memcpy(g, base[i % 3], 30);
        g[rng.below(30)] = static_cast<u8>(rng.below(49));
        g[rng.below(30)] = static_cast<u8>(rng.below(49));
        const double s = m.add(tick, hash_bytes(g, 30), g, 30, i % 2 == 0);
        if (i >= 290 && s >= 0)
            late_mutant = late_mutant > s ? late_mutant : s;
    }
    u8 novel[30];
    random_genome(rng, novel, 30);
    const double sn = m.add(tick, hash_bytes(novel, 30), novel, 30, true);
    CHECK(late_mutant > 0 && late_mutant < 3.0); // known code, two substitutions: some surprise
    CHECK(sn > 4.5);                             // a planted novel genome: near log2(49) = 5.61
    CHECK(m.add(tick + 1, hash_bytes(novel, 30), novel, 30, true) == -1.0); // a repeat
    m.finish();
    // Mutants at ticks 0-598 fill windows 0-5; the novel genome and its
    // repeat (ticks 600, 601) are window 6.
    CHECK(m.windows().size() == 7);
    const NoveltyWindow& last = m.windows()[m.windows().size() - 1];
    CHECK(last.first == 600 && last.saves == 2 && last.fresh == 1 && last.novel == 1 && last.novel_success == 1);
    CHECK(m.total().saves == 302 && m.windows()[0].saves == 50);
    printf("  novelty: a known mutant %.3f bits/codon, the planted novel genome %.3f\n", late_mutant, sn);
}

void test_novelty_windows() {
    NoveltyOptions o;
    o.window = 100;
    o.threshold = 1.5; // two symbols: an empty model gives 1 bit a symbol
    NoveltyMeter m;
    CHECK(m.init(2, o));
    const u8 a[4] = {0, 1, 0, 1}, b[4] = {1, 1, 1, 1};
    CHECK(close_to(m.add(5, 1, a, 4, true), 1.0, 1e-12)); // window 0, empty model
    CHECK(m.add(50, 1, a, 4, false) == -1.0);         // a repeat (same genotype)
    const double sb = m.add(150, 2, b, 4, false);     // window 1
    CHECK(sb > 0);
    m.add(420, 3, a, 4, true); // window 4; windows 2 and 3 empty
    m.finish();
    const Array<NoveltyWindow>& w = m.windows();
    CHECK(w.size() == 5);
    CHECK(w[0].first == 0 && w[0].last == 99 && w[0].saves == 2 && w[0].fresh == 1 && w[0].successful == 1);
    CHECK(w[1].first == 100 && w[1].saves == 1 && w[1].fresh == 1 && w[1].successful == 0);
    CHECK(w[2].saves == 0 && w[3].saves == 0 && w[3].first == 300);
    CHECK(w[4].first == 400 && w[4].fresh == 1 && w[4].successful == 1);
    CHECK(close_to(w[0].innovation(), 1.0, 1e-12) && w[0].novel == 0);
    CHECK(m.total().saves == 4 && m.total().fresh == 3);
}

// --- A4: alignment and neighbour joining --------------------------------------------------------

void test_alignment() {
    Rng rng = Rng::from_base(3);
    u8 a[40], rot[40], shifted[43], other[40], mut[40];
    random_genome(rng, a, 40);
    for (u32 j = 0; j < 40; ++j)
        rot[j] = a[(j + 40 - 7) % 40]; // a read from 7 codons later: a[i] = rot[(i + 7) % 40]
    shifted[0] = 5;
    shifted[1] = 6;
    shifted[2] = 7;
    memcpy(shifted + 3, a, 40);
    random_genome(rng, other, 40);
    for (u32 j = 0; j < 40; ++j)
        mut[j] = a[(j + 13) % 40];
    mut[20] = static_cast<u8>((mut[20] + 1) % 49);

    const Alignment r1 = align_cyclic(a, 40, rot, 40);
    CHECK(r1.cost == 0 && r1.offset == 7 && r1.distance == 0);
    CHECK(same_offset_similarity(a, 40, rot, 40) < 0.2); // the tracker's count misses it
    const Alignment r2 = align_cyclic(a, 40, shifted, 43);
    CHECK(r2.cost == 3);
    CHECK(same_offset_similarity(a, 40, shifted, 43) < 0.2);
    CHECK(align_cyclic(shifted, 43, a, 40).cost == 3);
    CHECK(align_cyclic(a, 40, mut, 40).cost == 1);
    CHECK(align_cyclic(mut, 40, a, 40).cost == 1);
    CHECK(align_cyclic(a, 40, other, 40).distance > 0.5);
    const u8 p[6] = {1, 2, 3, 4, 5, 6}, q[5] = {1, 9, 3, 4, 6};
    CHECK(edit_distance(p, 6, q, 5) == 2);
    CHECK(edit_distance(p, 6, p, 0) == 6);
    printf("  alignment: rotation cost %u (offset %u), 3-codon shift cost %u; unrelated %.2f\n", r1.cost, r1.offset,
           r2.cost, align_cyclic(a, 40, other, 40).distance);
}

void test_neighbour_joining() {
    // ((A:1,B:2):1.5,(C:1,D:3):0.5,(E:2,F:1):2), unrooted at the centre.
    const double leaf[6] = {1, 2, 1, 3, 2, 1};
    const int sub[6] = {0, 0, 1, 1, 2, 2};
    const double stem[3] = {1.5, 0.5, 2};
    Matrix d;
    d.resize(6, 6);
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            d.at(i, j) = i == j ? 0 : (sub[i] == sub[j] ? leaf[i] + leaf[j] : leaf[i] + stem[sub[i]] + stem[sub[j]] + leaf[j]);
    Tree t;
    neighbour_join(d, &t);
    CHECK(t.leaves == 6 && t.root >= 6);
    Array<double> dist;
    dist.resize(t.nodes.size());
    bool additive = true;
    for (u32 i = 0; i < 6; ++i) {
        tree_distances(t, i, dist.data());
        for (u32 j = 0; j < 6; ++j)
            additive = additive && close_to(dist[j], d.at(i, j), 1e-9);
    }
    CHECK(additive); // the tree's path lengths are the distances: the known tree
    CHECK(t.nodes[0].parent == t.nodes[1].parent && t.nodes[2].parent == t.nodes[3].parent &&
          t.nodes[4].parent == t.nodes[5].parent);
    const Str names[6] = {"A", "B", "C", "D", "E", "F"};
    Array<Str> labels;
    labels.resize(t.nodes.size());
    for (int i = 0; i < 6; ++i)
        labels[i] = names[i];
    TextBuf nw;
    tree_newick(t, labels.data(), nw);
    const Str s = nw.view();
    CHECK(s.n > 20 && s.p[s.n - 1] == ';' && s.find('A') < s.n && s.find('F') < s.n);
    printf("  neighbour joining: %.*s\n", static_cast<int>(s.n), s.p);
}

// --- A1: the Gaussian process -----------------------------------------------------------------

void test_lhs() {
    Matrix a, b, c;
    latin_hypercube(10, 3, 42, &a);
    latin_hypercube(10, 3, 42, &b);
    latin_hypercube(10, 3, 43, &c);
    bool strata = true;
    for (u32 j = 0; j < 3; ++j) {
        u32 seen[10] = {};
        for (u32 i = 0; i < 10; ++i) {
            const double v = a.at(i, j);
            strata = strata && v >= 0 && v < 1;
            ++seen[static_cast<u32>(v * 10)];
        }
        for (u32 k = 0; k < 10; ++k)
            strata = strata && seen[k] == 1;
    }
    CHECK(strata);
    CHECK(memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(double)) == 0);
    CHECK(memcmp(a.v.data(), c.v.data(), a.v.size() * sizeof(double)) != 0);
}

void test_gp_fit() {
    // 1D: sin(6x) from 12 points.
    Matrix x;
    x.resize(12, 1);
    double y[12];
    for (u32 i = 0; i < 12; ++i) {
        x.at(i, 0) = i / 11.0;
        y[i] = sin(6 * x.at(i, 0));
    }
    Gp gp;
    CHECK(gp.fit(x, y, true));
    double worst = 0;
    for (double q = 0.02; q < 1; q += 0.07) {
        double mu, sd;
        gp.predict(&q, &mu, &sd);
        worst = fabs(mu - sin(6 * q)) > worst ? fabs(mu - sin(6 * q)) : worst;
    }
    CHECK(worst < 0.05);
    double mu, sd_in, sd_out;
    const double at = 3.0 / 11.0, outside = 1.6;
    gp.predict(&at, &mu, &sd_in);
    gp.predict(&outside, &mu, &sd_out);
    CHECK(sd_in < 0.05 && sd_out > 5 * sd_in);
    printf("  gp 1D: worst error %.4f, length scale %.3f\n", worst, gp.length_scale(0));

    // 2D: sin(5u) + 0.5 cos(4v) from 30 spread points.
    Matrix x2;
    latin_hypercube(30, 2, 9, &x2);
    double y2[30];
    for (u32 i = 0; i < 30; ++i)
        y2[i] = sin(5 * x2.at(i, 0)) + 0.5 * cos(4 * x2.at(i, 1));
    Gp g2;
    CHECK(g2.fit(x2, y2, true));
    double worst2 = 0;
    const double probe[5][2] = {{0.1, 0.1}, {0.5, 0.5}, {0.9, 0.2}, {0.3, 0.8}, {0.7, 0.6}};
    for (const auto& pnt : probe) {
        double m2, s2;
        g2.predict(pnt, &m2, &s2);
        const double f = sin(5 * pnt[0]) + 0.5 * cos(4 * pnt[1]);
        worst2 = fabs(m2 - f) > worst2 ? fabs(m2 - f) : worst2;
    }
    CHECK(worst2 < 0.15);
    printf("  gp 2D: worst error %.4f\n", worst2);

    // A setting that doesn't matter gets a long length scale (low influence).
    double y3[30];
    for (u32 i = 0; i < 30; ++i)
        y3[i] = sin(6 * x2.at(i, 0));
    Gp g3;
    CHECK(g3.fit(x2, y3, true));
    CHECK(g3.length_scale(1) > 3 * g3.length_scale(0));
    printf("  gp influence: length scales %.3f (used) and %.3f (ignored)\n", g3.length_scale(0), g3.length_scale(1));
}

double peak1(double x) {
    return exp(-(x - 0.7) * (x - 0.7) / 0.02);
}

void test_gp_ei() {
    // 1D: from 4 spread points, expected improvement on a grid finds the peak at 0.7.
    Matrix x;
    latin_hypercube(4, 1, 1, &x);
    Array<double> y;
    for (u32 i = 0; i < 4; ++i)
        y.push(peak1(x.at(i, 0)));
    for (u32 it = 0; it < 12; ++it) {
        Gp gp;
        gp.fit(x, y.data(), true);
        double best = y[0];
        for (usize i = 1; i < y.size(); ++i)
            best = y[i] > best ? y[i] : best;
        double bx = 0, be = -1;
        for (u32 g = 0; g <= 200; ++g) {
            const double q = g / 200.0;
            const double e = gp.expected_improvement(&q, best, 0.0);
            if (e > be) {
                be = e;
                bx = q;
            }
        }
        Matrix x2;
        x2.resize(x.rows + 1, 1);
        memcpy(x2.v.data(), x.v.data(), x.rows * sizeof(double));
        x2.at(x.rows, 0) = bx;
        x = nova::move(x2);
        y.push(peak1(bx));
    }
    usize bi = 0;
    for (usize i = 1; i < y.size(); ++i)
        if (y[i] > y[bi])
            bi = i;
    CHECK(close_to(x.at(bi, 0), 0.7, 0.02));

    // 2D: random candidates from the search's own stream; the optimum (0.25, 0.6).
    Matrix x2;
    latin_hypercube(8, 2, 4, &x2);
    Array<double> y2;
    auto f2 = [](const double* p) { return -((p[0] - 0.25) * (p[0] - 0.25) + (p[1] - 0.6) * (p[1] - 0.6)); };
    for (u32 i = 0; i < 8; ++i)
        y2.push(f2(x2.row(i)));
    Rng rng = Rng::from_base(8);
    for (u32 it = 0; it < 15; ++it) {
        Gp gp;
        gp.fit(x2, y2.data(), true);
        double best = y2[0];
        for (usize i = 1; i < y2.size(); ++i)
            best = y2[i] > best ? y2[i] : best;
        double bp[2] = {0, 0}, be = -1;
        for (u32 c = 0; c < 2000; ++c) {
            const double p[2] = {rng.unit(), rng.unit()};
            const double e = gp.expected_improvement(p, best, 0.0);
            if (e > be) {
                be = e;
                bp[0] = p[0];
                bp[1] = p[1];
            }
        }
        Matrix x3;
        x3.resize(x2.rows + 1, 2);
        memcpy(x3.v.data(), x2.v.data(), x2.v.size() * sizeof(double));
        x3.at(x2.rows, 0) = bp[0];
        x3.at(x2.rows, 1) = bp[1];
        x2 = nova::move(x3);
        y2.push(f2(bp));
    }
    usize b2 = 0;
    for (usize i = 1; i < y2.size(); ++i)
        if (y2[i] > y2[b2])
            b2 = i;
    CHECK(close_to(x2.at(b2, 0), 0.25, 0.05) && close_to(x2.at(b2, 1), 0.6, 0.05));
    printf("  expected improvement: 1D best at %.3f (peak 0.7), 2D best at (%.3f, %.3f) (optimum 0.25, 0.6)\n",
           x.at(bi, 0), x2.at(b2, 0), x2.at(b2, 1));
}

// --- run files ----------------------------------------------------------------------------------

void test_line_reader() {
    const char* path = "nova_test_analysis_lines.txt";
    Array<char> big;
    big.resize(3u << 20);
    big.fill('x');
    TextBuf t;
    t.append("a\nbb\r\n");
    t.append(Str(big.data(), big.size()));
    t.append("\nccc"); // a last line still being written
    write_file(path, t.view().p, t.size());
    LineReader r;
    CHECK(r.open(path));
    Str line;
    CHECK(r.next(&line) && line.eq("a"));
    CHECK(r.next(&line) && line.eq("bb"));
    CHECK(r.next(&line) && line.n == big.size() && line.p[0] == 'x');
    CHECK(!r.next(&line));
    r.close();
    remove(path);
    CsvHeader h;
    CHECK(h.parse("tick,bodies, extra ,founder_origin"));
    CHECK(h.find("bodies") == 1 && h.find("extra") == 2 && h.find("founder_origin") == 3 && h.find("nope") == -1);
    u64 id = 0;
    CHECK(parse_lineage_id("610.12", &id) && id == ((610ull << 32) | 12));
    TextBuf ids;
    append_lineage_id(ids, id);
    CHECK(ids.view().eq("610.12"));
}

// A fake run folder: lineages.csv with an extra column, a genebank, stats with
// extra columns, result.txt.
void test_scores_files() {
    const char* dir = "nova_test_analysis_run";
    CreateDirectoryA(dir, nullptr);
    put("nova_test_analysis_run/run.cfg", "world.nodes = 4\nisa.table = nova_v2\n");
    put("nova_test_analysis_run/lineages.csv",
        "lineage,node,founded,ended,status,max_depth,peak_alive,members,founder_len,divide_links,parasite_links,"
        "gpu_links,founder_genotype,detected,founder_origin\n"
        "1.1,1,0,100,extinct,3,2,5,8,4,0,0,00000000000000aa,1,spark\n"
        "1.1,2,50,300,extinct,5,2,5,8,4,0,0,00000000000000aa,0,colonist\n"
        "2.1,2,10,30,extinct,1,1,1,8,0,0,0,00000000000000bb,0,spark\n"
        "3.1,3,100,200,alive,2,2,3,8,2,0,0,00000000000000cc,1,spark\n"
        "4.1,4,0,1000,alive,9,2,3,8,2,0,0,00000000000000dd,1,spark\n");
    put("nova_test_analysis_run/genebank.txt",
        "# Nova genebank\n"
        "0005aaa tick=10 node=1 lineage=1.1 parent=none hash=00000000000000a1 parent_hash=-\n"
        "  NOP0 SENSE ALLOC COPY DIVIDE\n"
        "0005aab tick=20 node=1 lineage=1.1 parent=0005aaa hash=00000000000000a2 parent_hash=00000000000000a1\n"
        "  NOP0 SENSE ALLOC COPY COPY\n"
        "0005aaa tick=30 node=2 lineage=2.1 parent=none hash=00000000000000b1 parent_hash=-\n"
        "  INERT INERT SENSE NOP1 DIVIDE\n"
        "0005aaa tick=40 node=3 lineage=1.1 parent=unsaved hash=00000000000000a2 parent_hash=00000000000000a1\n"
        "  NOP0 SENSE ALLOC COPY COPY\n"
        "0006aaa tick=600 node=3 lineage=3.1 parent=none hash=00000000000000c1 parent_hash=-\n"
        "  JMP JMP RET GPU YIELD EMIT\n"
        "0006aab tick=700 node=3 lineage=3.1 parent=0006aaa hash=00000000000000c2 parent_hash=00000000000000c1\n");
    put("nova_test_analysis_run/stats.csv", "tick_first,tick_last,ticks,lineages,bodies,new_column\n"
                                            "0,249,250,1,10,7\n250,499,250,1,3,7\n500,749,250,2,5,7\n"
                                            "750,999,250,2,8,7\n");
    put("nova_test_analysis_run/result.txt", "seed = 1\nend_tick = 1000\nstop = ticks\nseconds = 2\n"
                                             "hash = 00000000deadbeef\n");
    RunScores r;
    TextBuf err;
    ScoreOptions o;
    CHECK(score_run(dir, o, &r, &err));
    CHECK(r.ticks == 1000 && r.hash == 0xdeadbeef && r.gated == 3);
    CHECK(r.s[kScoreLifespan] == 300); // spans 300 (two nodes), 100, 1000
    CHECK(r.s[kScoreDepth] == 9);
    CHECK(r.s[kScoreGenotypes] == 4); // a1 a2 b1 c1 (a2 again on node 3; c2's genome isn't written yet)
    const double h = -(0.5 * log(0.5) + 2 * 0.25 * log(0.25));
    CHECK(close_to(r.s[kScoreEvenness], h / log(3.0), 1e-12));
    CHECK(r.body_floor == 4 && r.s[kScorePersistence] == 0.75 && r.min_bodies == 3);
    CHECK(r.s[kScoreSpeed] == 500);
    // Novelty: only c1 is gated and saved in the second half (tick >= 500):
    // its surprise per 1,000 ticks of the second half.
    NgramModel m;
    m.init(49, 4);
    Config cfg;
    Config::ParseError pe;
    cfg.parse("isa.table = nova_v2\n", &pe);
    CodonTable table;
    CHECK(run_codon_table(cfg, &table, &err));
    OpAlphabet abc;
    abc.init(table);
    CHECK(abc.size() == 49);
    Array<u8> s;
    const char* const gs[4] = {"NOP0 SENSE ALLOC COPY DIVIDE", "NOP0 SENSE ALLOC COPY COPY",
                               "INERT INERT SENSE NOP1 DIVIDE", "JMP JMP RET GPU YIELD EMIT"};
    double sc = 0;
    for (int i = 0; i < 4; ++i) {
        CHECK(abc.encode(gs[i], &s));
        if (i == 3)
            sc = m.surprise(s.data(), static_cast<u32>(s.size()));
        m.learn(s.data(), static_cast<u32>(s.size()));
    }
    CHECK(close_to(r.s[kScoreNovelty], sc * 1000.0 / 500.0, 1e-9));
    CHECK(!abc.encode("NOP0 NOTANOP", &s));

    // replay.log's replicator events add the living lineages lineages.csv lacks.
    put("nova_test_analysis_run/replay.log", "nova-replay 6\nconfig seed = 1\nhash 0 0000000000000001\n"
                                             "event 5 gpu_replicator node=2 lineage=7.1 depth=2 alive=2\n"
                                             "event 5 replicator node=2 lineage=2.1 depth=2 alive=2 len=5\n");
    GatedSet g;
    CHECK(load_gated(dir, &g, &err));
    CHECK(g.contains((2ull << 32) | 1) && g.contains((1ull << 32) | 1) && !g.contains((7ull << 32) | 1));
    CHECK(g.from_csv == 3 && g.from_replay == 1);
    // The novelty task over the same folder.
    NoveltyOptions no;
    no.window = 500;
    NoveltyReport nr;
    CHECK(novelty_analyse(dir, no, &nr, &err));
    CHECK(nr.windows.size() == 2 && nr.total.fresh == 4 && nr.total.saves == 5 && nr.alphabet == 49);
    CHECK(nr.windows[1].successful == 1 && close_to(nr.windows[1].success_surprise, sc, 1e-12));
    CHECK(path_inside("nova_test_analysis_run\\sub", dir) && !path_inside("nova_test_analysis_other", dir));
    remove_tree(dir);
}

// A lineage with a founder, rotated and shifted descendants, and an earlier
// near relative of the founder from another lineage.
void test_tree_synthetic() {
    const char* dir = "nova_test_analysis_tree";
    CreateDirectoryA(dir, nullptr);
    put("nova_test_analysis_tree/run.cfg", "isa.table = nova_v2\n");
    Config cfg;
    Config::ParseError pe;
    cfg.parse("isa.table = nova_v2\n", &pe);
    CodonTable table;
    TextBuf err;
    CHECK(run_codon_table(cfg, &table, &err));
    // Symbols to names: the table's distinct ops in codon order.
    const char* names[64];
    u32 nn = 0;
    for (u32 c = 0; c < 64; ++c) {
        const char* n = op_name(table.op[c]);
        bool dup = false;
        for (u32 k = 0; k < nn; ++k)
            dup = dup || strcmp(names[k], n) == 0;
        if (!dup)
            names[nn++] = n;
    }
    Rng rng = Rng::from_base(21);
    u8 g0[30], g1[30], g2[30], g3[32], p[30], a[30], b[30];
    random_genome(rng, g0, 30);
    memcpy(p, g0, 30);
    p[4] = static_cast<u8>((p[4] + 1) % 49);
    p[19] = static_cast<u8>((p[19] + 2) % 49);
    memcpy(g1, g0, 30);
    g1[10] = static_cast<u8>((g1[10] + 3) % 49);
    for (u32 j = 0; j < 30; ++j)
        g2[j] = g1[(j + 3) % 30]; // rotated
    g2[0] = static_cast<u8>((g2[0] + 5) % 49);
    g3[0] = 7;
    g3[1] = 8;
    memcpy(g3 + 2, g0, 30); // shifted
    random_genome(rng, a, 30);
    random_genome(rng, b, 30);
    TextBuf t;
    t.append("# Nova genebank\n");
    auto entry = [&](const char* name, u64 tick, u32 node, const char* lin, const char* parent, u64 hash, u64 ph,
                     const u8* g, u32 n) {
        char head[256];
        snprintf(head, sizeof head, "%s tick=%llu node=%u lineage=%s parent=%s hash=%016llx parent_hash=", name,
                 static_cast<unsigned long long>(tick), node, lin, parent, static_cast<unsigned long long>(hash));
        t.append(head);
        if (ph)
            t.append_hex64(ph);
        else
            t.append("-");
        t.append("\n ");
        for (u32 i = 0; i < n; ++i) {
            t.append_char(' ');
            t.append(names[g[i]]);
        }
        t.append_char('\n');
    };
    entry("0030aaa", 10, 1, "1.1", "none", 0xA, 0, a, 30);
    entry("0030aaa", 50, 4, "4.1", "none", 0xB, 0, p, 30);
    entry("0030aab", 60, 1, "1.1", "0030aaa", 0xC, 0xA, b, 30);
    entry("0030aaa", 100, 5, "5.1", "none", 0x10, 0, g0, 30);
    entry("0030aab", 120, 5, "5.1", "0030aaa", 0x11, 0x10, g1, 30);
    entry("0030aac", 140, 5, "5.1", "0030aab", 0x12, 0x11, g2, 30);
    entry("0032aaa", 150, 5, "5.1", "0030aaa", 0x13, 0x10, g3, 32);
    write_file("nova_test_analysis_tree/genebank.txt", t.view().p, t.size());

    TreeOptions o;
    o.by_lineage = true;
    o.lineage = (5ull << 32) | 1;
    o.neighbours = 1;
    TreeReport r;
    CHECK(tree_analyse(dir, o, &r, &err));
    CHECK(r.leaves == 5 && r.candidates == 4); // the lineage's 4 and its founder's nearest earlier genotype
    CHECK(r.links == 3 && r.aligned_agree == 3 && r.agree3 == 3);
    CHECK(r.founders == 1 && r.linked == 1 && r.mean_confidence > 0.5);
    const Str csv = r.links_csv.view();
    // The founder's likely ancestor is 4.1's genotype (p), two edits away.
    bool found = false;
    for (usize i = 0; i + 10 < csv.n && !found; ++i)
        found = Str(csv.p + i, 10).eq("0030aaa_n4") && i > 0 && csv.p[i - 1] == ',';
    CHECK(found);
    const Str nw = r.newick.view();
    CHECK(nw.n > 0 && nw.find(';') < nw.n);
    printf("  tree: %u leaves, %u of %u tracker links agree along the tree, founder linked with confidence %.2f\n",
           r.leaves, r.agree, r.links, r.mean_confidence);

    TreeOptions so;
    so.sample = 4;
    TreeReport rs;
    CHECK(tree_analyse(dir, so, &rs, &err));
    CHECK(rs.leaves >= 2 && rs.candidates == 7);
    remove_tree(dir);
}

// --- A1: a tiny real search -----------------------------------------------------------------------

// The base config the trials run: kSoupWorld1b at 8 nodes, with the legacy
// defaults written in (a child `nova run` has no fallback defaults), so it
// runs on the CPU only.
void write_base(const char* path) {
    Config c;
    Config::ParseError pe;
    c.parse(kSoupWorld1b, &pe);
    c.set("world.nodes", "8");
    c.set("world.chords", "4");
    c.set("detect.enabled", "true");
    Config legacy;
    legacy.parse(kLegacyDefaults, &pe);
    for (usize i = 0; i < legacy.count(); ++i)
        if (!c.has(legacy.key(i)))
            c.set(legacy.key(i), legacy.value(i));
    // `nova run` refuses a key nothing reads: keep the legacy keys this world reads.
    Config::set_fallback(nullptr, &pe);
    for (int pass = 0; pass < 64; ++pass) {
        TextBuf t;
        c.write(t);
        Run r;
        r.config().parse(t.view(), &pe);
        TextBuf err;
        Str unused;
        if (!r.init(1, &err) || !r.config().first_unused(&unused))
            break;
        Config d;
        for (usize i = 0; i < c.count(); ++i)
            if (!c.key(i).eq(unused))
                d.set(c.key(i), c.value(i));
        c = nova::move(d);
    }
    Config::set_fallback(kLegacyDefaults, &pe);
    TextBuf t;
    c.write(t);
    write_file(path, t.view().p, t.size());
}

bool nova_exe(char* out, usize cap) {
    const DWORD n = GetModuleFileNameA(nullptr, out, static_cast<DWORD>(cap));
    if (n == 0 || n >= cap)
        return false;
    char* slash = strrchr(out, '\\');
    if (!slash)
        return false;
    snprintf(slash + 1, cap - static_cast<usize>(slash + 1 - out), "nova.exe");
    return path_exists(out);
}

// summary.csv without its speed column (the clock's).
void summary_without_speed(const char* dir, TextBuf& out) {
    TextBuf p;
    path_join(p, dir, "summary.csv");
    LineReader r;
    if (!r.open(p.c_str()))
        return;
    Str line;
    i32 speed = -1;
    bool first = true;
    while (r.next(&line)) {
        Str f[64];
        const u32 n = csv_split(line, f, 64);
        if (first) {
            CsvHeader h;
            h.parse(line);
            speed = h.find("speed");
            first = false;
        }
        for (u32 i = 0; i < n; ++i)
            if (static_cast<i32>(i) != speed) {
                out.append(f[i]);
                out.append_char(',');
            }
        out.append_char('\n');
    }
}

void test_search() {
    char exe[MAX_PATH];
    if (!nova_exe(exe, sizeof exe)) {
        fprintf(stderr, "  search: nova.exe isn't beside the test program; build the nova target\n");
        CHECK(false);
        return;
    }
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    const char* root = "nova_test_analysis_search";
    remove_tree(root);
    CreateDirectoryA(root, nullptr);
    char base[MAX_PATH], file[MAX_PATH];
    snprintf(base, sizeof base, "%s\\%s\\base.cfg", cwd, root);
    write_base(base);
    TextBuf outs[2];
    for (int k = 0; k < 2; ++k) {
        char out[MAX_PATH];
        snprintf(out, sizeof out, "%s\\%s\\out%d", cwd, root, k);
        snprintf(file, sizeof file, "%s\\%s\\search%d.cfg", cwd, root, k);
        TextBuf sf;
        sf.append("# a tiny search\nsearch.base = ");
        sf.append(base);
        sf.append("\nsearch.out = ");
        sf.append(out);
        sf.append("\nsearch.seed = 7\nsearch.trials = 5\nsearch.init = 3\nsearch.seeds = 1\nsearch.parallel = 2\n"
                  "search.threads = 1\nsearch.ticks = 150\nrange.spark.rate = 0.05 1 log\n"
                  "range.cpu.wear = 0.0005 0.005\nrange.detect.genebank_threshold = 2 4\nfloor.genotypes = 1\n");
        write_file(file, sf.view().p, sf.size());
        SearchSpec s;
        TextBuf err;
        CHECK(search_load(file, &s, &err));
        if (err.size())
            fprintf(stderr, "  search_load: %s\n", err.c_str());
        SearchOptions o;
        o.nova_exe = exe;
        o.quiet = true;
        CHECK(search_run(s, o, &err));
        if (err.size())
            fprintf(stderr, "  search_run: %s\n", err.c_str());
        summary_without_speed(out, outs[k]);
    }
    // The same search file, the same trials, choices and scores.
    CHECK(outs[0].size() > 100 && outs[0].view().eq(outs[1].view()));
    const Str sum = outs[0].view();
    u32 ok_rows = 0, ei_rows = 0;
    for (usize i = 0; i + 4 < sum.n; ++i) {
        ok_rows += Str(sum.p + i, 4).eq(",ok,");
        ei_rows += Str(sum.p + i, 4).eq(",ei,");
    }
    CHECK(ok_rows == 5 && ei_rows == 2);
    printf("  search: 5 trials twice, identical summaries (%u bytes without speed)\n",
           static_cast<unsigned>(outs[0].size()));

    // A trial's run.cfg reproduces its run's hash: in this process, at
    // another thread count, without the test runner's fallback defaults.
    char trial[MAX_PATH];
    snprintf(trial, sizeof trial, "%s\\%s\\out0\\trial_004\\seed_1", cwd, root);
    RunScores rs;
    TextBuf err;
    CHECK(score_run(trial, ScoreOptions{}, &rs, &err));
    TextBuf cfgp;
    path_join(cfgp, trial, "run.cfg");
    Config::ParseError pe;
    Config::set_fallback(nullptr, &pe);
    Run run;
    CHECK(run.config().load_file(cfgp.c_str(), &pe));
    CHECK(run.init(3, &err));
    run.run_until(150);
    const u64 h = run.world().hash();
    Config::set_fallback(kLegacyDefaults, &pe);
    CHECK(rs.hash != 0);
    CHECK_EQ_U64(h, rs.hash);
    printf("  trial 4's run.cfg replays to its hash %016llx\n", static_cast<unsigned long long>(h));

    // The rest of the outputs.
    TextBuf p;
    path_join(p, trial, "..\\..\\best_1.cfg");
    CHECK(path_exists(p.c_str()));
    p.clear();
    snprintf(trial, sizeof trial, "%s\\%s\\out0", cwd, root);
    path_join(p, trial, "influence.csv");
    CHECK(path_exists(p.c_str()));
    p.clear();
    path_join(p, trial, "pareto.csv");
    CHECK(path_exists(p.c_str()));

    // Bad search files are refused, naming the problem.
    const char* const bad[] = {"range.cpu.wear = 0.5 2\n",       "range.seed = 1 5\n",   "range.no.such = 1 2\n",
                               "range.spark.rate = registry\n", "score.beauty = 1\n",   "bogus = 1\n",
                               "range.output.csv = 1 2\n",      "set.run.ticks = 5\n"};
    for (const char* b : bad) {
        TextBuf sf;
        sf.append("search.base = ");
        sf.append(base);
        sf.append("\nsearch.out = x\n");
        sf.append(b);
        write_file(file, sf.view().p, sf.size());
        SearchSpec s;
        TextBuf e2;
        CHECK(!search_load(file, &s, &e2) && e2.size() > 0);
    }
    remove_tree(root);
}

void test_objective() {
    SearchSpec s;
    s.weight[kScoreLifespan] = 1;
    s.weight[kScoreNovelty] = 1;
    s.floor[kScoreGenotypes] = 10;
    s.floor[kScorePersistence] = 0.9;
    s.floor_penalty = 2;
    double sc[kScoreCount] = {};
    sc[kScoreLifespan] = 99;
    sc[kScoreNovelty] = 1;
    sc[kScoreGenotypes] = 20;
    sc[kScorePersistence] = 1;
    CHECK(close_to(search_objective(s, sc), log(100.0) + log(2.0), 1e-12));
    sc[kScoreGenotypes] = 5; // half the diversity floor
    CHECK(close_to(search_objective(s, sc), log(100.0) + log(2.0) - 1.0, 1e-12));
    // Values snap to what a config can hold.
    SearchRange r;
    r.key = key_find("spark.rate");
    r.lo = 0.01;
    r.hi = 1;
    r.log = true;
    TextBuf v;
    double snapped = 0;
    search_value(r, 0.5, v, &snapped);
    CHECK(v.view().eq("0.1") && close_to(snapped, 0.5, 1e-12));
    r.key = key_find("detect.genebank_threshold");
    r.lo = 2;
    r.hi = 4;
    r.log = false;
    v.clear();
    search_value(r, 0.7, v, &snapped);
    CHECK(v.view().eq("3") && snapped == 0.5);
}

} // namespace

void suite_analysis() {
    test_cholesky();
    test_pca();
    test_kmeans();
    test_ngram_hand();
    test_novelty_planted();
    test_novelty_windows();
    test_alignment();
    test_neighbour_joining();
    test_lhs();
    test_gp_fit();
    test_gp_ei();
    test_line_reader();
    test_scores_files();
    test_tree_synthetic();
    test_objective();
    test_search();
}
