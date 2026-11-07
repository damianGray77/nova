#include "nova/landscape.h"

#include <cmath>   // floor, sqrt, exp, log, cos
#include <cstring> // memcpy

#include "nova/graph.h"
#include "nova/hashmap.h"
#include "nova/rng.h"

namespace nova {

namespace {

const u32 kNoMass = 0xFFFFFFFFu;
const u32 kNo = 0xFFFFFFFFu;
const double kPi = 3.14159265358979323846;

// A u64 whose unsigned order is the double's numeric order.
u64 order_key(double d) {
    u64 b;
    memcpy(&b, &d, sizeof b);
    return (b >> 63) ? ~b : (b | 0x8000000000000000ull);
}

usize count_below(const Array<u64>& sorted, u64 k) {
    usize lo = 0, hi = sorted.size();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (sorted[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

u64 pair_key(u32 a, u32 b) { return a < b ? (static_cast<u64>(a) << 32) | b : (static_cast<u64>(b) << 32) | a; }

// A standard normal draw (Box-Muller).
double normal(Rng& r) {
    double u1 = r.unit();
    u1 = u1 > 0 ? u1 : 1e-300;
    const double u2 = r.unit();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * kPi * u2);
}

// The standard normal quantile (Acklam's rational approximation, |error| < 1.2e-9).
double probit(double p) {
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                               1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                               6.680131188771972e+01,  -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                               -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                               3.754408661907416e+00};
    const double lo = 0.02425, hi = 1 - lo;
    if (p < lo) {
        const double q = sqrt(-2 * log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    if (p > hi) {
        const double q = sqrt(-2 * log(1 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    const double q = p - 0.5, r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
}

struct Tri {
    u32 v[3];  // counter-clockwise
    u32 nb[3]; // the neighbour opposite v[i], or kNo
    u32 mark;
    u32 alive;
};

} // namespace

// --- the triangulation ------------------------------------------------------------

void delaunay(const double* px, const double* py, u32 n, Array<u32>* tris) {
    tris->clear();
    if (n < 3)
        return;
    double minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    for (u32 i = 1; i < n; ++i) {
        minx = px[i] < minx ? px[i] : minx;
        maxx = px[i] > maxx ? px[i] : maxx;
        miny = py[i] < miny ? py[i] : miny;
        maxy = py[i] > maxy ? py[i] : maxy;
    }
    const double cx = 0.5 * (minx + maxx), cy = 0.5 * (miny + maxy);
    double span = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
    span = span > 1e-9 ? span : 1.0;
    const double S = 64.0 * span;
    Array<double> X, Y;
    X.assign(px, n);
    Y.assign(py, n);
    // A super triangle far around every point (counter-clockwise).
    X.push(cx - S);
    Y.push(cy - S);
    X.push(cx + S);
    Y.push(cy - S);
    X.push(cx);
    Y.push(cy + S);
    auto orient = [&](u32 a, u32 b, u32 c) {
        return (X[b] - X[a]) * (Y[c] - Y[a]) - (Y[b] - Y[a]) * (X[c] - X[a]);
    };
    auto incircle = [&](const Tri& t, u32 p) {
        const double ax = X[t.v[0]] - X[p], ay = Y[t.v[0]] - Y[p];
        const double bx = X[t.v[1]] - X[p], by = Y[t.v[1]] - Y[p];
        const double qx = X[t.v[2]] - X[p], qy = Y[t.v[2]] - Y[p];
        const double det = (ax * ax + ay * ay) * (bx * qy - qx * by) - (bx * bx + by * by) * (ax * qy - qx * ay) +
                           (qx * qx + qy * qy) * (ax * by - bx * ay);
        return det > 0;
    };
    Array<Tri> T;
    Array<u32> free_slots, bad, stack, made;
    struct Edge {
        u32 a, b, out, from;
    };
    Array<Edge> boundary;
    T.push(Tri{{n, n + 1, n + 2}, {kNo, kNo, kNo}, 0, 1});
    u32 last = 0, stamp = 0;
    for (u32 p = 0; p < n; ++p) {
        // 1. Walk from the last new triangle to the one holding p.
        u32 t = last;
        for (u32 step = 0; step < 4 * T.size() + 64; ++step) {
            const Tri& tr = T[t];
            u32 next = kNo;
            for (u32 k = 0; k < 3; ++k) {
                const u32 i = (k + step) % 3; // a varying start: no cycling
                if (orient(tr.v[(i + 1) % 3], tr.v[(i + 2) % 3], p) < 0) {
                    next = tr.nb[i];
                    break;
                }
            }
            if (next == kNo)
                break;
            t = next;
        }
        // 2. The cavity: every triangle connected to t whose circumcircle holds p.
        ++stamp;
        bad.clear();
        stack.clear();
        T[t].mark = stamp;
        stack.push(t);
        while (!stack.empty()) {
            const u32 u = stack.back();
            stack.pop();
            bad.push(u);
            for (u32 k = 0; k < 3; ++k) {
                const u32 nb = T[u].nb[k];
                if (nb != kNo && T[nb].mark != stamp && incircle(T[nb], p)) {
                    T[nb].mark = stamp;
                    stack.push(nb);
                }
            }
        }
        // 3. Its boundary, each edge with the triangle outside it.
        boundary.clear();
        for (usize i = 0; i < bad.size(); ++i) {
            const Tri& b = T[bad[i]];
            for (u32 k = 0; k < 3; ++k) {
                const u32 nb = b.nb[k];
                if (nb == kNo || T[nb].mark != stamp)
                    boundary.push(Edge{b.v[(k + 1) % 3], b.v[(k + 2) % 3], nb, bad[i]});
            }
        }
        // 4. A fan of new triangles from p to the boundary. (This insertion's
        //    own slots are reused only by later ones: an outside triangle may
        //    still point at several of them while the fan is linked.)
        made.clear();
        for (usize i = 0; i < boundary.size(); ++i) {
            const Edge& e = boundary[i];
            u32 slot;
            if (!free_slots.empty()) {
                slot = free_slots.back();
                free_slots.pop();
            } else {
                slot = static_cast<u32>(T.size());
                T.push(Tri{});
            }
            T[slot] = Tri{{p, e.a, e.b}, {e.out, kNo, kNo}, 0, 1};
            if (e.out != kNo)
                for (u32 j = 0; j < 3; ++j)
                    if (T[e.out].nb[j] == e.from) {
                        T[e.out].nb[j] = slot;
                        break;
                    }
            made.push(slot);
        }
        for (usize i = 0; i < made.size(); ++i) {
            Tri& ti = T[made[i]];
            for (usize j = 0; j < made.size(); ++j) {
                const Tri& tj = T[made[j]];
                if (tj.v[1] == ti.v[2])
                    ti.nb[1] = made[j]; // across (b, p)
                if (tj.v[2] == ti.v[1])
                    ti.nb[2] = made[j]; // across (p, a)
            }
        }
        for (usize i = 0; i < bad.size(); ++i) {
            T[bad[i]].alive = 0;
            free_slots.push(bad[i]);
        }
        last = made[0];
    }
    for (usize i = 0; i < T.size(); ++i) {
        const Tri& t = T[i];
        if (t.alive && t.v[0] < n && t.v[1] < n && t.v[2] < n)
            tris->append(t.v, 3);
    }
}

// --- distributions ---------------------------------------------------------------------

void spread_weights(u64 seed, u32 field, const Spread& s, const Array<double>& x, const Array<double>& y,
                    double width, Array<double>* out) {
    const usize n = x.size();
    out->resize(n);
    if (n == 0)
        return;
    Rng r(seed, Rng::kWorldNode, 0x7370726561640000ull + field); // "spread"
    Array<double> z;
    const double smooth = s.smooth < 0 ? 0 : (s.smooth > 1 ? 1 : s.smooth);
    const bool against = s.against != 0xFFFFFFFFu;
    if (smooth > 0)
        node_field(seed, against ? s.against : field, x, y, &z); // rank-normalised to -1..1
    for (usize i = 0; i < n; ++i) {
        double zf = 0;
        if (smooth > 0) {
            double u = (z[i] + 1) * 0.5;
            if (against) // rich where the other is poor
                u = 1 - u;
            const double edge = 0.5 / static_cast<double>(n);
            u = u < edge ? edge : (u > 1 - edge ? 1 - edge : u);
            zf = probit(u);
        }
        const double zr = normal(r);
        const double zz = smooth * zf + sqrt(1 - smooth * smooth) * zr;
        (*out)[i] = exp(s.spread * zz - 0.5 * s.spread * s.spread);
    }
    // Hotspots: clusters of plenty around random nodes.
    if (s.hotspots > 0 && s.boost > 0) {
        Array<u32> centre;
        for (u32 k = 0; k < s.hotspots; ++k)
            centre.push(static_cast<u32>(r.below(n)));
        const double sigma = 0.25 * sqrt(width / s.hotspots);
        for (usize i = 0; i < n; ++i) {
            double f = 0;
            for (u32 k = 0; k < s.hotspots; ++k) {
                const double dx = x[i] - x[centre[k]], dy = y[i] - y[centre[k]];
                f += exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
            }
            (*out)[i] *= 1.0 + s.boost * f;
        }
    }
    // Barren: the lowest-weighted nodes (ties by id) get none.
    const usize barren = static_cast<usize>(floor(s.barren * static_cast<double>(n) + 0.5));
    if (barren > 0) {
        // (the weight's order key with its low 20 bits replaced by the id:
        // weights compared to about 2^-32 relative, then by id)
        Array<u64> keys;
        for (usize i = 0; i < n; ++i)
            keys.push((order_key((*out)[i]) & ~0xFFFFFull) | i);
        sort_u64(keys.data(), n);
        for (usize i = 0; i < barren && i < n; ++i)
            (*out)[keys[i] & 0xFFFFFull] = 0;
    }
    double sum = 0;
    for (usize i = 0; i < n; ++i)
        sum += (*out)[i];
    const double scale = sum > 0 ? static_cast<double>(n) / sum : 0;
    for (usize i = 0; i < n; ++i)
        (*out)[i] *= scale;
}

void spread_total(const Array<double>& w, u64 total, u64 minimum, Array<u64>* out) {
    const usize n = w.size();
    out->resize(n);
    if (n == 0)
        return;
    const u64 base = minimum * n <= total ? minimum : total / n;
    const u64 rest = total - base * n;
    double sum = 0;
    for (usize i = 0; i < n; ++i)
        sum += w[i];
    if (sum <= 0) { // every node barren: the rest evenly
        for (usize i = 0; i < n; ++i)
            (*out)[i] = base + rest / n + (i < rest % n ? 1 : 0);
        return;
    }
    // Largest remainders (ties by id) make the shares add up exactly.
    u64 given = 0;
    Array<u64> rem;
    for (usize i = 0; i < n; ++i) {
        const double exact = static_cast<double>(rest) * w[i] / sum;
        const u64 whole = static_cast<u64>(floor(exact));
        (*out)[i] = base + whole;
        given += whole;
        const double frac = exact - static_cast<double>(whole);
        rem.push((~order_key(frac) & ~0xFFFFFull) | i); // largest fraction first, then id
    }
    sort_u64(rem.data(), n);
    for (u64 k = 0; given < rest && k < n; ++k, ++given)
        ++(*out)[rem[k] & 0xFFFFFull];
}

// --- substrates (docs/world-plan.md W1, W2) ----------------------------------------

const char* substrate_label(u32 type) {
    static const char* const n[kSubstrateTypes] = {"P cores",     "E cores",     "GPU",       "NPU",
                                                   "fast memory", "slow memory", "fast disk", "slow disk"};
    return type < kSubstrateTypes ? n[type] : "?";
}

const char* substrate_column(u32 type) {
    static const char* const n[kSubstrateTypes] = {"p_cores",  "e_cores",  "gpu",       "npu",
                                                   "mem_fast", "mem_slow", "disk_fast", "disk_slow"};
    return type < kSubstrateTypes ? n[type] : "?";
}

double substrate_multiplier(u32 level, double floor_mult) {
    return level == 0 ? floor_mult : 0.5 * level; // the floor, then 0.5, 1, 1.5, 2
}

void substrate_counts(const SubstrateParams& s, u32 n, u32* counts) {
    double sum = 0;
    for (u32 l = 0; l < kSubstrateLevels; ++l)
        sum += s.share[l] > 0 ? s.share[l] : 0;
    u32 given = 0;
    double frac[kSubstrateLevels];
    for (u32 l = 0; l < kSubstrateLevels; ++l) {
        const double exact = sum > 0 ? static_cast<double>(n) * (s.share[l] > 0 ? s.share[l] : 0) / sum
                                     : (l == kSubstrateOne ? static_cast<double>(n) : 0.0);
        counts[l] = static_cast<u32>(floor(exact));
        frac[l] = exact - counts[l];
        given += counts[l];
    }
    // The rest one each, largest remainder first (ties to the lower level).
    while (given < n) {
        u32 best = 0;
        for (u32 l = 1; l < kSubstrateLevels; ++l)
            if (frac[l] > frac[best])
                best = l;
        ++counts[best];
        frac[best] = -1;
        ++given;
    }
}

void substrate_levels(u64 seed, const SubstrateParams& s, const Array<double>& x, const Array<double>& y,
                      double time, Array<u8>* out) {
    const usize n = x.size();
    out->resize(n * kSubstrateTypes);
    if (n == 0)
        return;
    NOVA_ASSERT(n <= 0xFFFFF); // node ids fit the rank keys' low 20 bits (world.nodes is at most 10^6)
    u32 counts[kSubstrateLevels];
    substrate_counts(s, static_cast<u32>(n), counts);
    const double smooth = s.smooth < 0 ? 0 : (s.smooth > 1 ? 1 : s.smooth);
    const double scale = s.scale > 0 ? s.scale : 3.0;
    Array<u64> keys, sorted, rank;
    keys.resize(n);
    rank.resize(n);
    for (u32 t = 0; t < kSubstrateTypes; ++t) {
        // The type's field at each node, rank-normalised (as node_field), so
        // its spread doesn't depend on the raw noise's.
        if (smooth > 0) {
            if (time < 0) {
                Noise2 f;
                f.init(seed, kFieldSubstrate + t);
                for (usize i = 0; i < n; ++i)
                    keys[i] = order_key(f.fbm(x[i], y[i], 4, scale));
            } else {
                Noise3 f;
                f.init(seed, kFieldSubstrate + t);
                for (usize i = 0; i < n; ++i)
                    keys[i] = order_key(f.fbm(x[i], y[i], time, 4, scale));
            }
            sorted.assign(keys.data(), n);
            sort_u64(sorted.data(), n);
        }
        // Mixed with an independent draw per node, as a distribution is
        // (spread_weights): smooth x field + sqrt(1 - smooth^2) x its own.
        Rng r(seed, Rng::kWorldNode, 0x7375627300000000ull + t); // "subs"
        for (usize i = 0; i < n; ++i) {
            double zf = 0;
            if (smooth > 0) {
                double u = n > 1 ? static_cast<double>(count_below(sorted, keys[i])) / static_cast<double>(n - 1) : 0.5;
                const double edge = 0.5 / static_cast<double>(n);
                u = u < edge ? edge : (u > 1 - edge ? 1 - edge : u);
                zf = probit(u);
            }
            const double zr = normal(r);
            const double zz = smooth * zf + sqrt(1 - smooth * smooth) * zr;
            rank[i] = (order_key(zz) & ~0xFFFFFull) | i; // (ties by id)
        }
        // By rank: the lowest counts[0] nodes take the floor, and so on up.
        sort_u64(rank.data(), n);
        usize k = 0;
        for (u32 l = 0; l < kSubstrateLevels; ++l)
            for (u32 c = 0; c < counts[l] && k < n; ++c, ++k)
                (*out)[(rank[k] & 0xFFFFFull) * kSubstrateTypes + t] = static_cast<u8>(l);
    }
}

// --- the landscape -----------------------------------------------------------------------

bool generate_landscape(u64 seed, const LandParams& p, Landscape* L, TextBuf* err) {
    const u32 N = p.nodes;
    const u32 h = p.resolution < 16 ? 16 : p.resolution;
    u32 w = static_cast<u32>(floor(h * p.aspect + 0.5));
    w = w < 1 ? 1 : w;
    L->raster_w = w;
    L->raster_h = h;
    L->width = static_cast<double>(w) / h;
    const double W = L->width;
    const usize cells = static_cast<usize>(w) * h;

    // --- land and water ---------------------------------------------------------
    Noise2 terrain, wx, wy;
    terrain.init(seed, kFieldTerrain);
    wx.init(seed, kFieldWarpX);
    wy.init(seed, kFieldWarpY);
    Array<u64> keys, sorted;
    keys.resize(cells);
    for (u32 j = 0; j < h; ++j) {
        for (u32 i = 0; i < w; ++i) {
            const double x = (i + 0.5) / h, y = (j + 0.5) / h;
            const double qx = x + p.warp * wx.fbm(x, y, 3, p.scale);
            const double qy = y + p.warp * wy.fbm(x, y, 3, p.scale);
            double v = terrain.fbm(qx, qy, 5, p.scale);
            double e = x;
            e = W - x < e ? W - x : e;
            e = y < e ? y : e;
            e = 1 - y < e ? 1 - y : e;
            e /= 0.2; // 0 at the edge, 1 a fifth of the way in
            const double f = e < 1 ? 1 - e : 0;
            v -= p.border * f * f;
            keys[static_cast<usize>(j) * w + i] = order_key(v);
        }
    }
    sorted.assign(keys.data(), cells);
    sort_u64(sorted.data(), cells);
    const double frac = p.fraction < 0 ? 0 : (p.fraction > 1 ? 1 : p.fraction);
    const usize cut = static_cast<usize>(floor((1.0 - frac) * static_cast<double>(cells) + 0.5));
    const u64 thr = cut < cells ? sorted[cut] : ~0ull;
    L->land.resize(cells);
    usize land_cells = 0;
    for (usize c = 0; c < cells; ++c) {
        L->land[c] = keys[c] >= thr && cut < cells ? 1 : 0;
        land_cells += L->land[c];
    }
    if (land_cells == 0) {
        err->append("the landscape has no land (raise land.fraction)");
        return false;
    }

    // --- landmasses: a flood fill over 4-neighbours ----------------------------
    L->cell_mass.resize(cells);
    L->cell_mass.fill(kNoMass);
    L->masses = 0;
    Array<u32> st;
    for (usize c0 = 0; c0 < cells; ++c0) {
        if (!L->land[c0] || L->cell_mass[c0] != kNoMass)
            continue;
        const u32 m = L->masses++;
        st.clear();
        st.push(static_cast<u32>(c0));
        L->cell_mass[c0] = m;
        while (!st.empty()) {
            const u32 c = st.back();
            st.pop();
            const u32 ci = c % w, cj = c / w;
            const u32 nbs[4] = {ci > 0 ? c - 1 : kNo, ci + 1 < w ? c + 1 : kNo, cj > 0 ? c - w : kNo,
                                cj + 1 < h ? c + w : kNo};
            for (u32 k = 0; k < 4; ++k) {
                const u32 q = nbs[k];
                if (q != kNo && L->land[q] && L->cell_mass[q] == kNoMass) {
                    L->cell_mass[q] = m;
                    st.push(q);
                }
            }
        }
    }

    // --- nodes: darts thrown onto land, none closer than r ------------------
    Array<u32> land_list;
    for (usize c = 0; c < cells; ++c)
        if (L->land[c])
            land_list.push(static_cast<u32>(c));
    const double area = static_cast<double>(land_cells) / (static_cast<double>(h) * h);
    double r = sqrt(0.5 * area / (N ? N : 1));
    Rng rng(seed, Rng::kWorldNode, 0x6C616E6400000000ull); // "land"
    Array<double> px, py;
    Array<u32> grid;
    for (;;) {
        // Nodes closer than a quarter of a raster cell would sit on coastlines
        // the raster can't draw: ask for a finer raster or more land instead.
        if (r < 0.25 / h) {
            err->append("world.nodes don't fit on the land: lower world.nodes, or raise land.fraction or "
                        "land.resolution");
            return false;
        }
        const double gs = r / 1.41421356237;
        const u32 gw = static_cast<u32>(ceil(W / gs)) + 1, gh = static_cast<u32>(ceil(1.0 / gs)) + 1;
        grid.resize(static_cast<usize>(gw) * gh);
        grid.fill(kNo);
        auto gcell = [&](double x, double y, u32* gx, u32* gy) {
            *gx = static_cast<u32>(x / gs);
            *gy = static_cast<u32>(y / gs);
            *gx = *gx >= gw ? gw - 1 : *gx;
            *gy = *gy >= gh ? gh - 1 : *gy;
        };
        for (usize i = 0; i < px.size(); ++i) {
            u32 gx, gy;
            gcell(px[i], py[i], &gx, &gy);
            grid[static_cast<usize>(gy) * gw + gx] = static_cast<u32>(i);
        }
        const u64 budget = 60ull * N + 1000;
        for (u64 a = 0; a < budget && px.size() < N; ++a) {
            const u32 c = land_list[static_cast<usize>(rng.below(land_list.size()))];
            const double x = (c % w + rng.unit()) / h, y = (c / w + rng.unit()) / h;
            u32 gx, gy;
            gcell(x, y, &gx, &gy);
            bool ok = true;
            for (i32 dy = -2; dy <= 2 && ok; ++dy) {
                for (i32 dx = -2; dx <= 2 && ok; ++dx) {
                    const i32 qx = static_cast<i32>(gx) + dx, qy = static_cast<i32>(gy) + dy;
                    if (qx < 0 || qy < 0 || qx >= static_cast<i32>(gw) || qy >= static_cast<i32>(gh))
                        continue;
                    const u32 o = grid[static_cast<usize>(qy) * gw + qx];
                    if (o == kNo)
                        continue;
                    const double ex = px[o] - x, ey = py[o] - y;
                    ok = ex * ex + ey * ey >= r * r;
                }
            }
            if (!ok)
                continue;
            grid[static_cast<usize>(gy) * gw + gx] = static_cast<u32>(px.size());
            px.push(x);
            py.push(y);
        }
        if (px.size() >= N)
            break;
        r *= 0.85;
    }
    L->spacing = r;
    // Numbered row by row (rows about a spacing apart), left to right.
    {
        const double rows = floor(1.0 / (2 * r)) < 1 ? 1 : floor(1.0 / (2 * r));
        Array<u64> order;
        for (u32 i = 0; i < N; ++i) {
            const u64 row = static_cast<u64>(py[i] * rows);
            const u64 xq = static_cast<u64>(px[i] / W * 16777215.0);
            order.push((row << 44) | (xq << 20) | i);
        }
        sort_u64(order.data(), N);
        L->x.resize(N);
        L->y.resize(N);
        for (u32 k = 0; k < N; ++k) {
            const u32 i = static_cast<u32>(order[k] & 0xFFFFF);
            L->x[k] = px[i];
            L->y[k] = py[i];
        }
    }
    auto cell_of = [&](double x, double y) {
        u32 ci = static_cast<u32>(x * h), cj = static_cast<u32>(y * h);
        ci = ci >= w ? w - 1 : ci;
        cj = cj >= h ? h - 1 : cj;
        return static_cast<usize>(cj) * w + ci;
    };
    L->mass.resize(N);
    for (u32 i = 0; i < N; ++i)
        L->mass[i] = L->cell_mass[cell_of(L->x[i], L->y[i])];

    // --- the triangulation ---------------------------------------------------------
    Array<u32> tris;
    delaunay(L->x.data(), L->y.data(), N, &tris);
    L->tri_edges.clear();
    for (usize t = 0; t + 2 < tris.size(); t += 3)
        for (u32 k = 0; k < 3; ++k)
            L->tri_edges.push(pair_key(tris[t + k], tris[t + (k + 1) % 3]));
    sort_u64(L->tri_edges.data(), L->tri_edges.size());
    {
        usize u = 0;
        for (usize i = 0; i < L->tri_edges.size(); ++i)
            if (u == 0 || L->tri_edges[i] != L->tri_edges[u - 1])
                L->tri_edges[u++] = L->tri_edges[i];
        L->tri_edges.resize(u);
    }
    auto len = [&](u32 a, u32 b) {
        const double dx = L->x[a] - L->x[b], dy = L->y[a] - L->y[b];
        return sqrt(dx * dx + dy * dy);
    };
    // Does the straight path a-b stay on a's landmass all the way?
    auto on_land = [&](u32 a, u32 b) {
        const double l = len(a, b);
        const u32 steps = static_cast<u32>(ceil(l * h * 2.0)) + 1;
        for (u32 s = 0; s <= steps; ++s) {
            const double t = static_cast<double>(s) / steps;
            const double x = L->x[a] + (L->x[b] - L->x[a]) * t, y = L->y[a] + (L->y[b] - L->y[a]) * t;
            if (L->cell_mass[cell_of(x, y)] != L->mass[a])
                return false;
        }
        return true;
    };
    // Land candidates and crossings, each sorted by length (then key).
    Array<u64> land_c, cross_c, bay_c; // (order key of the length's top 40 bits | index into tri_edges)
    L->dropped_water = 0;
    for (usize i = 0; i < L->tri_edges.size(); ++i) {
        const u32 a = static_cast<u32>(L->tri_edges[i] >> 32), b = static_cast<u32>(L->tri_edges[i]);
        const u64 k = (order_key(len(a, b)) & ~0xFFFFFFull) | i;
        if (L->mass[a] == L->mass[b] && on_land(a, b)) {
            land_c.push(k);
        } else if (L->mass[a] != L->mass[b]) {
            if (len(a, b) <= p.bridge_max_length)
                cross_c.push(k);
            ++L->dropped_water;
        } else {
            bay_c.push(k); // same landmass, but across a bay
            ++L->dropped_water;
        }
    }
    sort_u64(bay_c.data(), bay_c.size());
    NOVA_ASSERT(L->tri_edges.size() < (1u << 24));
    sort_u64(land_c.data(), land_c.size());
    sort_u64(cross_c.data(), cross_c.size());

    // --- how many paths each node wants ----------------------------------------
    Rng pr(seed, Rng::kWorldNode, 0x7061746873000000ull); // "paths"
    L->want.resize(N);
    L->hermits = L->hubs = 0;
    Array<u8> hub;
    hub.resize(N);
    const u32 pmax = p.paths_max < 1 ? 1 : p.paths_max;
    for (u32 i = 0; i < N; ++i) {
        const double u = pr.unit();
        const double z = normal(pr);
        if (u < p.paths_isolated) {
            L->want[i] = 0;
            ++L->hermits;
        } else if (u < p.paths_isolated + p.paths_hubs) {
            L->want[i] = pmax;
            hub[i] = 1;
            ++L->hubs;
        } else {
            double d = floor(p.paths_mean * exp(p.paths_spread * z - 0.5 * p.paths_spread * p.paths_spread) + 0.5);
            d = d < 1 ? 1 : (d > pmax ? pmax : d);
            L->want[i] = static_cast<u32>(d);
        }
    }
    Array<u32> deg;
    deg.resize(N);
    HashMap<u32> kept; // undirected pair -> 1 land path, 2 long, 3 bridge
    auto keep = [&](u32 a, u32 b, u32 kind) {
        bool inserted = false;
        u32& v = kept.get_or_insert(pair_key(a, b), &inserted);
        if (!inserted)
            return false;
        v = kind;
        ++deg[a];
        ++deg[b];
        return true;
    };
    // Each landmass in one piece: its spanning tree first (Kruskal).
    if (p.paths_connected) {
        Array<u32> parent;
        parent.resize(N);
        for (u32 i = 0; i < N; ++i)
            parent[i] = i;
        auto find = [&](u32 v) {
            while (parent[v] != v) {
                parent[v] = parent[parent[v]];
                v = parent[v];
            }
            return v;
        };
        // Over land paths, then (for a winding landmass whose only straight
        // links cross a bay) the shortest crossings of its own bays.
        for (u32 pass = 0; pass < 2; ++pass) {
            const Array<u64>& c = pass == 0 ? land_c : bay_c;
            for (usize i = 0; i < c.size(); ++i) {
                const u64 e = L->tri_edges[c[i] & 0xFFFFFF];
                const u32 a = static_cast<u32>(e >> 32), b = static_cast<u32>(e);
                const u32 ra = find(a), rb = find(b);
                if (ra == rb)
                    continue;
                parent[ra] = rb;
                keep(a, b, 1);
            }
        }
        // Pieces still apart (the triangulation reached them only through
        // another landmass): each joins the nearest node of its landmass
        // outside it. Rare, and done once.
        Array<u32> first_of;
        first_of.resize(L->masses);
        first_of.fill(kNo);
        for (u32 i = 0; i < N; ++i)
            if (first_of[L->mass[i]] == kNo)
                first_of[L->mass[i]] = i;
        for (bool merged = true; merged;) {
            merged = false;
            for (u32 i = 0; i < N; ++i) {
                const u32 m = L->mass[i];
                if (find(i) == find(first_of[m]))
                    continue;
                u32 best = kNo;
                double best_d = 0;
                for (u32 j = 0; j < N; ++j) {
                    if (L->mass[j] != m || find(j) == find(i))
                        continue;
                    const double d = len(i, j);
                    if (best == kNo || d < best_d) {
                        best = j;
                        best_d = d;
                    }
                }
                if (best != kNo) {
                    parent[find(i)] = find(best);
                    keep(i, best, 1);
                    merged = true;
                }
            }
        }
    }
    // Shortest first, while both ends want another.
    for (usize i = 0; i < land_c.size(); ++i) {
        const u64 e = L->tri_edges[land_c[i] & 0xFFFFFF];
        const u32 a = static_cast<u32>(e >> 32), b = static_cast<u32>(e);
        if (deg[a] < L->want[a] && deg[b] < L->want[b])
            keep(a, b, 1);
    }
    // Hubs: roads to their neighbours' neighbours (on their own landmass).
    {
        Array<u32> begin, adj;
        begin.resize(static_cast<usize>(N) + 1);
        for (usize i = 0; i < L->tri_edges.size(); ++i) {
            ++begin[(L->tri_edges[i] >> 32) + 1];
            ++begin[(L->tri_edges[i] & 0xFFFFFFFFull) + 1];
        }
        for (u32 i = 0; i < N; ++i)
            begin[i + 1] += begin[i];
        Array<u32> fill;
        fill.assign(begin.data(), N);
        adj.resize(begin[N]);
        for (usize i = 0; i < L->tri_edges.size(); ++i) {
            const u32 a = static_cast<u32>(L->tri_edges[i] >> 32), b = static_cast<u32>(L->tri_edges[i]);
            adj[fill[a]++] = b;
            adj[fill[b]++] = a;
        }
        Array<u64> cand;
        for (u32 a = 0; a < N; ++a) {
            if (!hub[a] || deg[a] >= L->want[a])
                continue;
            cand.clear();
            for (u32 j = begin[a]; j < begin[a + 1]; ++j) {
                const u32 m = adj[j];
                for (u32 k = begin[m]; k < begin[m + 1]; ++k) {
                    const u32 b = adj[k];
                    if (b != a && L->mass[b] == L->mass[a] && L->want[b] > 0 && !kept.contains(pair_key(a, b)))
                        cand.push((order_key(len(a, b)) & ~0xFFFFFull) | b);
                }
                if (L->mass[m] == L->mass[a] && L->want[m] > 0 && !kept.contains(pair_key(a, m)))
                    cand.push((order_key(len(a, m)) & ~0xFFFFFull) | m);
            }
            sort_u64(cand.data(), cand.size());
            for (usize i = 0; i < cand.size() && deg[a] < L->want[a]; ++i) {
                const u32 b = static_cast<u32>(cand[i] & 0xFFFFF);
                if (on_land(a, b))
                    keep(a, b, 1);
            }
        }
    }
    // Long links within a landmass: the farthest of 8 random same-landmass nodes.
    L->long_added = 0;
    const u32 want_long = static_cast<u32>(floor(p.paths_long * N + 0.5));
    for (u32 l = 0; l < want_long && N >= 3; ++l) {
        for (u32 attempt = 0; attempt < 32; ++attempt) {
            const u32 a = static_cast<u32>(pr.below(N));
            if (L->want[a] == 0)
                continue;
            u32 far = kNo;
            double far_d = -1;
            for (u32 c = 0; c < 8; ++c) {
                const u32 b = static_cast<u32>(pr.below(N));
                if (b == a || L->mass[b] != L->mass[a] || L->want[b] == 0)
                    continue;
                const double d = len(a, b);
                if (d > far_d) {
                    far_d = d;
                    far = b;
                }
            }
            if (far != kNo && keep(a, far, 2)) {
                ++L->long_added;
                break;
            }
        }
    }
    // Bridges: per pair of neighbouring landmasses, the shortest crossings
    // with no endpoint shared, up to bridge.count; isolated landmasses get none.
    Array<u8> lonely;
    lonely.resize(L->masses);
    Rng br(seed, Rng::kWorldNode, 0x6272696467650000ull); // "bridge"
    for (u32 m = 0; m < L->masses; ++m)
        lonely[m] = br.chance(p.bridge_isolated) ? 1 : 0;
    L->bridges = 0;
    if (p.bridge_count > 0) {
        HashMap<u32> per_pair;
        HashMap<u32> used_end;
        for (usize i = 0; i < cross_c.size(); ++i) {
            const u64 e = L->tri_edges[cross_c[i] & 0xFFFFFF];
            const u32 a = static_cast<u32>(e >> 32), b = static_cast<u32>(e);
            const u32 ma = L->mass[a], mb = L->mass[b];
            if (lonely[ma] || lonely[mb] || L->want[a] == 0 || L->want[b] == 0) // hermits want no path at all
                continue;
            u32& count = per_pair.get_or_insert(pair_key(ma, mb));
            if (count >= p.bridge_count || used_end.contains(a) || used_end.contains(b))
                continue;
            if (keep(a, b, 3)) {
                ++count;
                used_end.get_or_insert(a) = 1;
                used_end.get_or_insert(b) = 1;
                ++L->bridges;
            }
        }
    }

    // --- attributes, both directions, in key order ------------------------------
    Array<u64> und; // undirected kept pairs, sorted
    for (usize i = 0; i < kept.slot_count(); ++i)
        if (kept.slot_used(i))
            und.push(kept.slot_key(i));
    sort_u64(und.data(), und.size());
    Noise2 rug;
    rug.init(seed, kFieldRugged);
    Array<u64> rug_sorted;
    auto rug_key = [&](u32 a, u32 b) {
        return order_key(rug.fbm((L->x[a] + L->x[b]) * 0.5, (L->y[a] + L->y[b]) * 0.5));
    };
    for (usize i = 0; i < und.size(); ++i)
        if (*kept.find(und[i]) != 3)
            rug_sorted.push(rug_key(static_cast<u32>(und[i] >> 32), static_cast<u32>(und[i])));
    sort_u64(rug_sorted.data(), rug_sorted.size());
    // Bridge phases, by their order.
    HashMap<u32> phase_of;
    {
        u32 bi = 0;
        for (usize i = 0; i < und.size(); ++i) {
            if (*kept.find(und[i]) != 3)
                continue;
            u32 ph = 0;
            if (p.open_every > 0) {
                if (p.phase == kPhaseStaggered)
                    ph = static_cast<u32>(static_cast<u64>(bi) * p.open_every / (L->bridges ? L->bridges : 1));
                else if (p.phase == kPhaseRandom)
                    ph = static_cast<u32>(br.below(p.open_every));
            }
            phase_of.get_or_insert(und[i]) = ph;
            ++bi;
        }
    }
    L->keys.clear();
    for (usize i = 0; i < und.size(); ++i) {
        const u64 a = und[i] >> 32, b = und[i] & 0xFFFFFFFFull;
        L->keys.push((a << 32) | b);
        L->keys.push((b << 32) | a);
    }
    sort_u64(L->keys.data(), L->keys.size());
    const usize E = L->keys.size();
    L->latency.resize(E);
    L->bandwidth.resize(E);
    L->phase.resize(E);
    L->dist.resize(E);
    L->bridge.resize(E);
    const double count = static_cast<double>(rug_sorted.size());
    for (usize i = 0; i < E; ++i) {
        const u32 a = static_cast<u32>(L->keys[i] >> 32), b = static_cast<u32>(L->keys[i]);
        const u32 lo = a < b ? a : b, hi = a < b ? b : a;
        const u64 pk = pair_key(lo, hi);
        const u32 kind = *kept.find(pk);
        const double d = len(lo, hi);
        L->dist[i] = d;
        if (kind == 3) {
            L->bridge[i] = 1;
            L->latency[i] = 1 + static_cast<u32>(floor(d * p.bridge_latency_scale));
            L->bandwidth[i] = p.bridge_bandwidth ? p.bridge_bandwidth : 1;
            L->phase[i] = *phase_of.find(pk);
        } else {
            L->bridge[i] = 0;
            L->latency[i] = 1 + static_cast<u32>(floor(d * p.latency_scale));
            const double u = count > 0 ? static_cast<double>(count_below(rug_sorted, rug_key(lo, hi))) / count : 0.0;
            const double bw = floor(p.bandwidth * (1.0 - 0.75 * (u < 1.0 ? u : 1.0)));
            L->bandwidth[i] = bw < 1.0 ? 1u : static_cast<u32>(bw);
            L->phase[i] = 0;
        }
    }
    return true;
}

} // namespace nova
