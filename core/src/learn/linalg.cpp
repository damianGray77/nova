// Vectors, matrices and Cholesky solves (docs/analysis-plan.md A0).
#include <cmath> // sqrt, log

#include "nova/learn.h"

namespace nova {

double dot(const double* a, const double* b, usize n) {
    double s = 0;
    for (usize i = 0; i < n; ++i)
        s += a[i] * b[i];
    return s;
}

bool cholesky(Matrix& a) {
    NOVA_ASSERT(a.rows == a.cols);
    const usize n = a.rows;
    double* m = a.v.data();
    for (usize j = 0; j < n; ++j) {
        double* rj = m + j * n;
        double d = rj[j] - dot(rj, rj, j);
        if (!(d > 0))
            return false;
        d = sqrt(d);
        rj[j] = d;
        for (usize i = j + 1; i < n; ++i) {
            double* ri = m + i * n;
            ri[j] = (ri[j] - dot(ri, rj, j)) / d;
        }
        for (usize k = j + 1; k < n; ++k)
            rj[k] = 0;
    }
    return true;
}

void solve_lower(const Matrix& l, double* b) {
    const usize n = l.rows;
    const double* m = l.v.data();
    for (usize i = 0; i < n; ++i)
        b[i] = (b[i] - dot(m + i * n, b, i)) / m[i * n + i];
}

void solve_upper_t(const Matrix& l, double* b) {
    const usize n = l.rows;
    const double* m = l.v.data();
    for (usize i = n; i-- > 0;) {
        double s = b[i];
        for (usize k = i + 1; k < n; ++k)
            s -= m[k * n + i] * b[k];
        b[i] = s / m[i * n + i];
    }
}

void cholesky_solve(const Matrix& l, double* b) {
    solve_lower(l, b);
    solve_upper_t(l, b);
}

double cholesky_logdet(const Matrix& l) {
    double s = 0;
    for (usize i = 0; i < l.rows; ++i)
        s += log(l.at(i, i));
    return 2 * s;
}

} // namespace nova
