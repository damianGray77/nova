// k-means with k-means++ seeding (docs/analysis-plan.md A0, for A3).
#include "nova/learn.h"
#include "nova/rng.h"

namespace nova {

namespace {

double dist2(const double* a, const double* b, usize d) {
    double s = 0;
    for (usize j = 0; j < d; ++j) {
        const double t = a[j] - b[j];
        s += t * t;
    }
    return s;
}

} // namespace

double kmeans(const Matrix& x, u32 k, u64 seed, u32 max_iter, Matrix* centroids, Array<u32>* labels) {
    const usize n = x.rows, d = x.cols;
    if (k > n)
        k = static_cast<u32>(n);
    centroids->resize(k, d);
    labels->resize(n);
    if (n == 0 || k == 0)
        return 0;
    Rng rng = Rng::from_base(seed ^ 0x6B6D65616E732B2Bull); // "kmeans++"

    // k-means++: the first centre uniformly, each next one with probability
    // proportional to the squared distance to the nearest centre so far.
    Array<double> near;
    near.resize(n);
    {
        const usize first = static_cast<usize>(rng.below(n));
        memcpy(centroids->row(0), x.row(first), d * sizeof(double));
        for (usize i = 0; i < n; ++i)
            near[i] = dist2(x.row(i), centroids->row(0), d);
    }
    for (u32 c = 1; c < k; ++c) {
        double total = 0;
        for (usize i = 0; i < n; ++i)
            total += near[i];
        usize pick = 0;
        if (total > 0) {
            double r = rng.unit() * total;
            pick = n - 1;
            for (usize i = 0; i < n; ++i) {
                r -= near[i];
                if (r < 0) {
                    pick = i;
                    break;
                }
            }
        } else {
            pick = static_cast<usize>(rng.below(n));
        }
        memcpy(centroids->row(c), x.row(pick), d * sizeof(double));
        for (usize i = 0; i < n; ++i) {
            const double t = dist2(x.row(i), centroids->row(c), d);
            if (t < near[i])
                near[i] = t;
        }
    }

    // Lloyd's iterations.
    Array<u32> count;
    count.resize(k);
    u32* lab = labels->data();
    for (usize i = 0; i < n; ++i)
        lab[i] = 0xFFFFFFFFu;
    double inertia = 0;
    for (u32 it = 0; it < max_iter; ++it) {
        bool changed = false;
        inertia = 0;
        for (usize i = 0; i < n; ++i) {
            u32 best = 0;
            double bd = dist2(x.row(i), centroids->row(0), d);
            for (u32 c = 1; c < k; ++c) {
                const double t = dist2(x.row(i), centroids->row(c), d);
                if (t < bd) {
                    bd = t;
                    best = c;
                }
            }
            if (lab[i] != best) {
                lab[i] = best;
                changed = true;
            }
            near[i] = bd;
            inertia += bd;
        }
        if (!changed)
            break;
        // New centroids; an empty cluster takes the point farthest from its own.
        centroids->v.fill(0.0);
        count.fill(0);
        for (usize i = 0; i < n; ++i) {
            double* c = centroids->row(lab[i]);
            const double* r = x.row(i);
            for (usize j = 0; j < d; ++j)
                c[j] += r[j];
            ++count[lab[i]];
        }
        for (u32 c = 0; c < k; ++c) {
            double* cr = centroids->row(c);
            if (count[c]) {
                for (usize j = 0; j < d; ++j)
                    cr[j] /= static_cast<double>(count[c]);
                continue;
            }
            usize far = 0;
            for (usize i = 1; i < n; ++i)
                if (near[i] > near[far])
                    far = i;
            memcpy(cr, x.row(far), d * sizeof(double));
            near[far] = 0;
        }
    }
    return inertia;
}

} // namespace nova
