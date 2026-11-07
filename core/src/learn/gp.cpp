// A Gaussian process with expected improvement, and Latin hypercubes
// (docs/analysis-plan.md A1).
#include <cmath> // sqrt, exp, log, erfc

#include "nova/learn.h"
#include "nova/rng.h"

namespace nova {

namespace {

constexpr double kSqrt5 = 2.2360679774997896964;
constexpr double kLog2Pi = 1.8378770664093454836;

// Bounds on the log hyperparameters (inputs in [0, 1], values standardised).
constexpr double kLogEllLo = -3.912023005428146; // log 0.02
constexpr double kLogEllHi = 3.912023005428146;  // log 50
constexpr double kLogSfLo = -2.995732273553991;  // log 0.05
constexpr double kLogSfHi = 2.302585092994046;   // log 10
constexpr double kLogSnLo = -6.907755278982137;  // log 0.001
constexpr double kLogSnHi = 0.6931471805599453;  // log 2

double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

double normal_pdf(double z) {
    return 0.3989422804014327 * exp(-0.5 * z * z);
}

double normal_cdf(double z) {
    return 0.5 * erfc(-z * 0.7071067811865476);
}

void latin_hypercube(u32 n, u32 d, u64 seed, Matrix* out) {
    out->resize(n, d);
    if (n == 0)
        return;
    Rng rng = Rng::from_base(seed ^ 0x4C6174696E487970ull); // "LatinHyp"
    Array<u32> perm;
    perm.resize(n);
    for (u32 j = 0; j < d; ++j) {
        for (u32 i = 0; i < n; ++i)
            perm[i] = i;
        for (u32 i = n - 1; i > 0; --i) { // Fisher-Yates
            const u32 k = static_cast<u32>(rng.below(i + 1));
            const u32 t = perm[i];
            perm[i] = perm[k];
            perm[k] = t;
        }
        for (u32 i = 0; i < n; ++i)
            out->at(i, j) = (perm[i] + rng.unit()) / n;
    }
}

// The Matern 5/2 kernel with the hyperparameters factor() last set.
double Gp::kernel(const double* a, const double* b) const {
    double r2 = 0;
    const double* inv = inv_ell_.data();
    for (u32 k = 0; k < d_; ++k) {
        const double t = (a[k] - b[k]) * inv[k];
        r2 += t * t;
    }
    const double r = sqrt(r2);
    return sf2_ * (1 + kSqrt5 * r + (5.0 / 3.0) * r2) * exp(-kSqrt5 * r);
}

// Factors K + sn^2 I into l_ and solves alpha = K^-1 y. False if it fails.
bool Gp::factor(const double* theta) {
    const u32 n = n_;
    inv_ell_.resize(d_);
    for (u32 k = 0; k < d_; ++k)
        inv_ell_[k] = exp(-theta[k]);
    sf2_ = exp(2 * theta[d_]);
    l_.resize(n, n);
    const double sn2 = exp(2 * theta[d_ + 1]);
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = 0; j <= i; ++j) {
            const double k = kernel(x_.row(i), x_.row(j));
            l_.at(i, j) = k;
            l_.at(j, i) = k;
        }
        l_.at(i, i) += sn2 + 1e-9;
    }
    if (!cholesky(l_))
        return false;
    alpha_.resize(n);
    for (u32 i = 0; i < n; ++i)
        alpha_[i] = y_[i];
    cholesky_solve(l_, alpha_.data());
    return true;
}

double Gp::neg_log_lik(const double* theta) {
    if (!factor(theta))
        return 1e300;
    double fit = 0;
    for (u32 i = 0; i < n_; ++i)
        fit += y_[i] * alpha_[i];
    return 0.5 * fit + 0.5 * cholesky_logdet(l_) + 0.5 * n_ * kLog2Pi;
}

bool Gp::fit(const Matrix& x, const double* y, bool optimise) {
    const u32 n = static_cast<u32>(x.rows), d = static_cast<u32>(x.cols);
    const bool reshape = d != d_ || theta_.size() != d + 2;
    n_ = n;
    d_ = d;
    x_.resize(n, d);
    if (n)
        memcpy(x_.v.data(), x.v.data(), n * d * sizeof(double));
    // Standardise the values.
    double mean = 0;
    for (u32 i = 0; i < n; ++i)
        mean += y[i];
    mean = n ? mean / n : 0;
    double var = 0;
    for (u32 i = 0; i < n; ++i)
        var += (y[i] - mean) * (y[i] - mean);
    var = n > 1 ? var / (n - 1) : 0;
    if (optimise || reshape) {
        ymean_ = mean;
        yscale_ = var > 1e-24 ? sqrt(var) : 1.0;
    }
    y_.resize(n);
    for (u32 i = 0; i < n; ++i)
        y_[i] = (y[i] - ymean_) / yscale_;
    if (reshape) {
        theta_.resize(d + 2);
        for (u32 k = 0; k < d; ++k)
            theta_[k] = log(0.5);
        theta_[d] = 0;
        theta_[d + 1] = log(0.1);
    }
    if (n == 0)
        return true;
    if (optimise) {
        // Pattern search (Hooke and Jeeves' exploratory moves) on the logs,
        // from three starts; the best wins, ties to the earlier start.
        const u32 p = d + 2;
        Array<double> best, cur, trial;
        best.resize(p);
        cur.resize(p);
        trial.resize(p);
        double best_f = 1e300;
        const double starts[3] = {log(0.2), log(0.5), log(1.5)};
        for (double s0 : starts) {
            for (u32 k = 0; k < d; ++k)
                cur[k] = s0;
            cur[d] = 0;
            cur[d + 1] = log(0.1);
            double f = neg_log_lik(cur.data());
            double step = 1.0;
            u32 evals = 0;
            while (step > 0.01 && evals < 400 * p) {
                bool moved = false;
                for (u32 k = 0; k < p; ++k) {
                    const double lo = k < d ? kLogEllLo : (k == d ? kLogSfLo : kLogSnLo);
                    const double hi = k < d ? kLogEllHi : (k == d ? kLogSfHi : kLogSnHi);
                    for (int dir = 1; dir >= -1; dir -= 2) {
                        for (u32 q = 0; q < p; ++q)
                            trial[q] = cur[q];
                        trial[k] = clampd(cur[k] + dir * step, lo, hi);
                        if (trial[k] == cur[k])
                            continue;
                        const double ft = neg_log_lik(trial.data());
                        ++evals;
                        if (ft < f - 1e-12) {
                            f = ft;
                            cur[k] = trial[k];
                            moved = true;
                            break;
                        }
                    }
                }
                if (!moved)
                    step *= 0.5;
            }
            if (f < best_f) {
                best_f = f;
                for (u32 q = 0; q < p; ++q)
                    best[q] = cur[q];
            }
        }
        for (u32 q = 0; q < p; ++q)
            theta_[q] = best[q];
    }
    if (!factor(theta_.data()))
        return false;
    double fitv = 0;
    for (u32 i = 0; i < n_; ++i)
        fitv += y_[i] * alpha_[i];
    ll_ = -(0.5 * fitv + 0.5 * cholesky_logdet(l_) + 0.5 * n_ * kLog2Pi);
    return true;
}

void Gp::predict(const double* x, double* mean, double* sd) const {
    if (n_ == 0) {
        *mean = ymean_;
        *sd = yscale_ * exp(theta_.size() ? theta_[d_] : 0.0);
        return;
    }
    Array<double> k;
    k.resize(n_);
    for (u32 i = 0; i < n_; ++i)
        k[i] = kernel(x, x_.row(i));
    double mu = 0;
    for (u32 i = 0; i < n_; ++i)
        mu += k[i] * alpha_[i];
    solve_lower(l_, k.data());
    double var = sf2_ - dot(k.data(), k.data(), n_);
    if (var < 0)
        var = 0;
    *mean = ymean_ + yscale_ * mu;
    *sd = yscale_ * sqrt(var);
}

double Gp::expected_improvement(const double* x, double best, double xi) const {
    double mu, sd;
    predict(x, &mu, &sd);
    const double gain = mu - best - xi;
    if (sd < 1e-12)
        return gain > 0 ? gain : 0.0;
    const double z = gain / sd;
    return gain * normal_cdf(z) + sd * normal_pdf(z);
}

double Gp::length_scale(u32 i) const {
    return exp(theta_[i]);
}

double Gp::signal_sd() const {
    return yscale_ * exp(theta_[d_]);
}

double Gp::noise_sd() const {
    return yscale_ * exp(theta_[d_ + 1]);
}

} // namespace nova
