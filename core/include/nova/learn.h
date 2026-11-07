#pragma once

// The analysis library (docs/analysis-plan.md A0): small hand-rolled models
// for the tools that read what runs produce. Nothing here touches a world, and
// every random choice comes from a seed the caller gives (the analysis's own
// stream, never a world's), so every result repeats exactly.
//
//   Matrix, cholesky...    dense row-major matrices and Cholesky solves
//   pca                    principal components by power iteration (A5)
//   kmeans                 k-means with k-means++ seeding (A3)
//   NgramModel             n-gram counts with Witten-Bell smoothing (A2)
//   align_cyclic           edit distance that forgives a rotation (A4)
//   neighbour_join         a tree from distances (A4)
//   Gp, latin_hypercube    a Gaussian process, expected improvement (A1)
#include "nova/array.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

// --- vectors and matrices ----------------------------------------------------

// A dense row-major matrix of doubles.
struct Matrix {
    usize rows = 0, cols = 0;
    Array<double> v;

    // Sets the shape; every entry becomes 0.
    void resize(usize r, usize c) {
        rows = r;
        cols = c;
        v.resize(r * c);
        v.fill(0.0);
    }
    double& at(usize i, usize j) { return v[i * cols + j]; }
    double at(usize i, usize j) const { return v[i * cols + j]; }
    double* row(usize i) { return v.data() + i * cols; }
    const double* row(usize i) const { return v.data() + i * cols; }
};

double dot(const double* a, const double* b, usize n);

// A = L L^T for a symmetric positive definite n x n matrix, in place: the
// lower triangle becomes L and the upper is zeroed. False if a pivot is not
// positive (A is not positive definite); `a` is then left part-way.
bool cholesky(Matrix& a);
// With l from cholesky: L y = b (forward), L^T x = y (back), and both, which
// solves A x = b. b is overwritten with the answer.
void solve_lower(const Matrix& l, double* b);
void solve_upper_t(const Matrix& l, double* b);
void cholesky_solve(const Matrix& l, double* b);
// log det A = 2 sum log L_ii.
double cholesky_logdet(const Matrix& l);

// --- principal components ----------------------------------------------------

// Principal components of the rows of x (n samples of d values): the mean,
// k unit components as the rows of comps (k x d, the largest variance first)
// and the variance along each. Power iteration with deflation, never forming
// the d x d covariance, so d can be large (A5's 1,024 hashed 3-codon counts).
// Deterministic: fixed start vectors, and each component's sign is set so its
// largest entry is positive. k is capped at d.
void pca(const Matrix& x, u32 k, Array<double>* mean, Matrix* comps, Array<double>* variance, u32 max_iter = 2000,
         double tol = 1e-12);
// out[j] = (r - mean) . comps row j, for every component.
void pca_project(const double* r, const Array<double>& mean, const Matrix& comps, double* out);

// --- k-means -------------------------------------------------------------------

// k-means of the rows of x: k-means++ seeding drawn from `seed`, then Lloyd's
// iterations until no label changes (or max_iter). An emptied cluster restarts
// at the point farthest from its centroid. Fills centroids (k x d) and labels
// (n) and returns the inertia: the sum of squared distances from each point to
// its centroid. k is capped at n.
double kmeans(const Matrix& x, u32 k, u64 seed, u32 max_iter, Matrix* centroids, Array<u32>* labels);

// --- n-gram counts -------------------------------------------------------------

// An n-gram model of symbol sequences (genomes as op symbols, A2) with
// Witten-Bell interpolated smoothing: for a context h of k symbols,
//
//   P_k(s | h) = (c(h, s) + T(h) P_{k-1}(s | h')) / (N(h) + T(h))
//
// where c is how often s followed h, N(h) how often h was followed by
// anything, T(h) how many different symbols followed it, h' is h without its
// oldest symbol, and P_{-1} = 1 / alphabet. A context never seen gives way to
// the shorter one. Position i of a sequence is predicted from the
// min(i, order - 1) symbols before it. Counts are dense arrays, so an
// alphabet of 49 at order 4 takes about 24 MB.
class NgramModel {
  public:
    static constexpr u32 kMaxOrder = 4;
    static constexpr u32 kMaxAlphabet = 64;

    // Forgets everything. alphabet: symbols 0 .. alphabet - 1 (2 to 64);
    // order: the longest n-gram counted (1 to 4). False if out of range.
    bool init(u32 alphabet, u32 order = 4);

    // P(sym | the ctx_len symbols before it, oldest first; only the last
    // order - 1 of them matter).
    double prob(const u8* ctx, u32 ctx_len, u32 sym) const;
    // The bits needed to code s under the model as it stands (nothing is
    // learned), and the same per symbol: the surprise (0 when len is 0).
    double bits(const u8* s, u32 len) const;
    double surprise(const u8* s, u32 len) const { return len ? bits(s, len) / len : 0.0; }
    // Counts s's n-grams. Symbols must be below the alphabet.
    void learn(const u8* s, u32 len);

    u32 alphabet() const { return alphabet_; }
    u32 order() const { return order_; }
    u64 learned() const { return learned_; } // sequences learned

  private:
    u32 alphabet_ = 0, order_ = 0;
    u64 learned_ = 0;
    Array<u32> count_[kMaxOrder]; // order k (contexts of k symbols): V^(k+1) counts of (context, symbol)
    Array<u32> total_[kMaxOrder]; // ... V^k context totals N
    Array<u32> types_[kMaxOrder]; // ... V^k distinct followers T
};

// --- sequence alignment ----------------------------------------------------------

struct Alignment {
    u32 cost = 0;        // edits: substitutions, insertions and deletions, the rotation free
    u32 offset = 0;      // the rotation: where in the longer sequence the match starts
    double distance = 0; // cost / the longer length: 0 = the same up to rotation
};

// Reused between alignments, so a distance matrix doesn't allocate per pair.
struct AlignScratch {
    Array<u32> cost0, cost1, start0, start1;
    Array<u8> text;
};

// Edit distance that forgives a rotation (a genome read from another start)
// and tolerates shifts (codons inserted or lost, so the same code sits at
// another offset). The shorter sequence is aligned against the longer written
// twice, free to start and end anywhere; whatever of the longer the match
// leaves out or covers twice counts as edits too. For equal lengths both ways
// are tried and the cheaper kept, so the distance is symmetric. An upper
// bound on the true cyclic edit distance, exact for a pure rotation or a pure
// insertion.
Alignment align_cyclic(const u8* a, u32 n, const u8* b, u32 m, AlignScratch* scratch = nullptr);
// Plain edit distance (Levenshtein), for comparison.
u32 edit_distance(const u8* a, u32 n, const u8* b, u32 m);
// Same-offset matches over the longer length: what the lineage tracker counts
// (design.md 6.6), which a rotation or a shift defeats.
double same_offset_similarity(const u8* a, u32 n, const u8* b, u32 m);

// --- trees -----------------------------------------------------------------------

struct TreeNode {
    i32 parent = -1;   // -1 at the root
    double length = 0; // of the branch up to the parent
    i32 child[3] = {-1, -1, -1};
    u32 children = 0;
};

// Leaves are nodes 0 .. leaves - 1 (the rows of the distance matrix, in
// order); the joins follow; the root is the last node.
struct Tree {
    Array<TreeNode> nodes;
    i32 root = -1;
    u32 leaves = 0;
};

// Neighbour joining (Saitou and Nei 1987, in Studier and Keppler's form) of a
// symmetric n x n distance matrix: unrooted, so the last three nodes join at
// the root. Ties go to the lowest pair, so the tree is fully determined by d.
// A negative branch length is set to 0 and the difference moved to its
// sister. Grows as n^3: a few thousand leaves at most.
void neighbour_join(const Matrix& d, Tree* out);
// The tree in Newick form, leaf i named labels[i] (which must avoid the
// characters ( ) , : ; and spaces), branch lengths to 6 significant digits.
void tree_newick(const Tree& t, const Str* labels, TextBuf& out);
// Path lengths along the tree from node `from` to every node (dist: one per
// node).
void tree_distances(const Tree& t, u32 from, double* dist);

// --- Gaussian processes and expected improvement -------------------------------

double normal_pdf(double z);
double normal_cdf(double z);

// A Latin hypercube: n points in [0, 1)^d (out: n x d), exactly one in each
// of n equal slices of every axis, slices paired and placed at random from
// `seed`.
void latin_hypercube(u32 n, u32 d, u64 seed, Matrix* out);

// A Gaussian process for maximising a noisy function of a few settings (A1):
// a constant mean, a Matern 5/2 kernel with its own length scale for each
// input, and signal and noise variances. Inputs are scaled to [0, 1]; values
// are standardised inside, so any units work.
class Gp {
  public:
    // Fits to the rows of x and their values y. With `optimise`, the
    // hyperparameters maximise the marginal likelihood: a pattern search on
    // their logs from three fixed starts, so a fit is fully determined by its
    // data. Without, the current ones are kept (a believed point added while
    // picking a batch). False if the kernel matrix can't be factored.
    bool fit(const Matrix& x, const double* y, bool optimise = true);

    // The posterior mean and standard deviation at x, in y's units.
    void predict(const double* x, double* mean, double* sd) const;
    // Expected improvement over `best` (maximising) at x, in y's units; xi
    // asks for at least that much improvement.
    double expected_improvement(const double* x, double best, double xi = 0.0) const;

    u32 dims() const { return d_; }
    u32 points() const { return n_; }
    double length_scale(u32 i) const;
    double signal_sd() const; // in y's units
    double noise_sd() const;
    double log_likelihood() const { return ll_; }

  private:
    double neg_log_lik(const double* theta);
    bool factor(const double* theta);
    double kernel(const double* a, const double* b) const;

    u32 n_ = 0, d_ = 0;
    Matrix x_, l_;
    Array<double> y_, alpha_, theta_; // y standardised; theta = log length scales, log sf, log sn
    Array<double> inv_ell_;           // 1 / length scale, as factor() last set them
    double sf2_ = 1;                  // ... and the signal variance
    double ymean_ = 0, yscale_ = 1, ll_ = 0;
};

} // namespace nova
