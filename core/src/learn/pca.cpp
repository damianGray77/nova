// Principal components by power iteration (docs/analysis-plan.md A0, for A5).
#include <cmath> // sqrt, fabs

#include "nova/learn.h"

namespace nova {

void pca(const Matrix& x, u32 k, Array<double>* mean, Matrix* comps, Array<double>* variance, u32 max_iter,
         double tol) {
    const usize n = x.rows, d = x.cols;
    if (k > d)
        k = static_cast<u32>(d);
    mean->resize(d);
    mean->fill(0.0);
    comps->resize(k, d);
    variance->resize(k);
    variance->fill(0.0);
    if (n == 0 || d == 0)
        return;
    double* mu = mean->data();
    for (usize i = 0; i < n; ++i) {
        const double* r = x.row(i);
        for (usize j = 0; j < d; ++j)
            mu[j] += r[j];
    }
    for (usize j = 0; j < d; ++j)
        mu[j] /= static_cast<double>(n);

    Array<double> v, w, proj;
    v.resize(d);
    w.resize(d);
    proj.resize(n);
    for (u32 c = 0; c < k; ++c) {
        // A fixed start, different per component and never orthogonal to
        // everything: 1, 1.5, 0.75, ... folded by the component's index.
        for (usize j = 0; j < d; ++j)
            v[j] = 1.0 + 0.5 * static_cast<double>((j * 7 + c * 13) % 11) / 11.0;
        double lambda = 0;
        for (u32 it = 0; it < max_iter; ++it) {
            // w = Xc^T (Xc v), with earlier components projected out.
            for (usize i = 0; i < n; ++i) {
                const double* r = x.row(i);
                double s = 0;
                for (usize j = 0; j < d; ++j)
                    s += (r[j] - mu[j]) * v[j];
                proj[i] = s;
            }
            w.fill(0.0);
            for (usize i = 0; i < n; ++i) {
                const double* r = x.row(i);
                const double p = proj[i];
                for (usize j = 0; j < d; ++j)
                    w[j] += (r[j] - mu[j]) * p;
            }
            for (u32 e = 0; e < c; ++e) {
                const double* u = comps->row(e);
                const double s = dot(u, w.data(), d);
                for (usize j = 0; j < d; ++j)
                    w[j] -= s * u[j];
            }
            const double norm = sqrt(dot(w.data(), w.data(), d));
            if (!(norm > 0)) {
                lambda = 0;
                break;
            }
            double change = 0;
            for (usize j = 0; j < d; ++j) {
                const double nv = w[j] / norm;
                change += (nv - v[j]) * (nv - v[j]);
                v[j] = nv;
            }
            lambda = norm;
            if (change < tol)
                break;
        }
        // The sign: the largest entry positive.
        usize big = 0;
        for (usize j = 1; j < d; ++j)
            if (fabs(v[j]) > fabs(v[big]))
                big = j;
        const double sign = v[big] < 0 ? -1.0 : 1.0;
        double* out = comps->row(c);
        for (usize j = 0; j < d; ++j)
            out[j] = lambda > 0 ? sign * v[j] : 0.0;
        (*variance)[c] = n > 1 ? lambda / static_cast<double>(n - 1) : 0.0;
    }
}

void pca_project(const double* r, const Array<double>& mean, const Matrix& comps, double* out) {
    const usize d = comps.cols;
    for (usize c = 0; c < comps.rows; ++c) {
        const double* u = comps.row(c);
        double s = 0;
        for (usize j = 0; j < d; ++j)
            s += (r[j] - mean[j]) * u[j];
        out[c] = s;
    }
}

} // namespace nova
