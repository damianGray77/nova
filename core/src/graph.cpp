#include "nova/graph.h"

#include <cmath> // floor, sqrt, ceil

#include "nova/hashmap.h"
#include "nova/rng.h"

namespace nova {

namespace {

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
double lerp(double a, double b, double t) { return a + t * (b - a); }

// Eight gradients: the four diagonals and the four axes.
double grad(u8 h, double x, double y) {
    switch (h & 7) {
    case 0:
        return x + y;
    case 1:
        return -x + y;
    case 2:
        return x - y;
    case 3:
        return -x - y;
    case 4:
        return x;
    case 5:
        return -x;
    case 6:
        return y;
    default:
        return -y;
    }
}

// A u64 whose unsigned order is the double's numeric order (no NaNs here).
u64 order_key(double d) {
    u64 b;
    memcpy(&b, &d, sizeof b);
    return (b >> 63) ? ~b : (b | 0x8000000000000000ull);
}

// Index of the first key >= k in a sorted array: how many are below k.
usize count_below(const Array<u64>& sorted, u64 k) {
    usize lo = 0, hi = sorted.size();
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2;
        if (sorted[mid] < k)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

double dist2(const ProcGraph& g, u32 a, u32 b) {
    const double dx = g.x[a] - g.x[b], dy = g.y[a] - g.y[b];
    return dx * dx + dy * dy;
}

u64 pair_key(u32 a, u32 b) {
    return a < b ? (static_cast<u64>(a) << 32) | b : (static_cast<u64>(b) << 32) | a;
}

// A link's ruggedness: the field at its midpoint, as an order key.
u64 rug_key(const Noise2& rug, const ProcGraph& g, u32 a, u32 b) {
    return order_key(rug.fbm((g.x[a] + g.x[b]) * 0.5, (g.y[a] + g.y[b]) * 0.5));
}

// Each (live) site to its k nearest (live) sites, ties by id; the undirected
// pairs, sorted and unique.
void proximity(const ProcGraph& g, const GraphParams& gp, const u8* live, Array<u64>* out) {
    const u32 n = static_cast<u32>(g.x.size());
    u32 alive = 0;
    for (u32 i = 0; i < n; ++i)
        alive += !live || live[i];
    const u32 k = gp.links < alive ? gp.links : (alive ? alive - 1 : 0);
    out->clear();
    Array<u32> best;
    Array<double> best_d;
    for (u32 a = 0; a < n; ++a) {
        if (live && !live[a])
            continue;
        best.clear();
        best_d.clear();
        for (u32 b = 0; b < n; ++b) {
            if (b == a || (live && !live[b]))
                continue;
            const double d = dist2(g, a, b);
            usize at = best.size();
            while (at > 0 && best_d[at - 1] > d) // b > earlier ids, so ties keep order
                --at;
            if (at >= k)
                continue;
            best.insert(at, b);
            best_d.insert(at, d);
            if (best.size() > k) {
                best.pop();
                best_d.pop();
            }
        }
        for (usize i = 0; i < best.size(); ++i)
            out->push(pair_key(a, best[i]));
    }
    sort_u64(out->data(), out->size());
    usize unique = 0;
    for (usize i = 0; i < out->size(); ++i)
        if (unique == 0 || (*out)[i] != (*out)[unique - 1])
            (*out)[unique++] = (*out)[i];
    out->resize(unique);
}

// Twelve gradients: the edge midpoints of a cube.
double grad3(u8 h, double x, double y, double z) {
    switch (h % 12) {
    case 0:
        return x + y;
    case 1:
        return -x + y;
    case 2:
        return x - y;
    case 3:
        return -x - y;
    case 4:
        return x + z;
    case 5:
        return -x + z;
    case 6:
        return x - z;
    case 7:
        return -x - z;
    case 8:
        return y + z;
    case 9:
        return -y + z;
    case 10:
        return y - z;
    default:
        return -y - z;
    }
}

// Ranks values over the nodes: the lowest -1, the highest +1.
void rank_normalise(const Array<u64>& keys, Array<double>* out) {
    const usize n = keys.size();
    out->resize(n);
    if (n == 1) {
        (*out)[0] = 0;
        return;
    }
    Array<u64> sorted;
    sorted.assign(keys.data(), n);
    sort_u64(sorted.data(), n);
    for (usize i = 0; i < n; ++i)
        (*out)[i] = -1.0 + 2.0 * static_cast<double>(count_below(sorted, keys[i])) / static_cast<double>(n - 1);
}

} // namespace

void Noise3::init(u64 seed, u32 field) {
    Rng r(seed, Rng::kWorldNode, 0x6E6F697365330000ull + field); // "noise3"
    for (u32 i = 0; i < 256; ++i)
        perm_[i] = static_cast<u8>(i);
    for (u32 i = 255; i > 0; --i) {
        const u32 j = static_cast<u32>(r.below(i + 1));
        const u8 t = perm_[i];
        perm_[i] = perm_[j];
        perm_[j] = t;
    }
    for (u32 i = 0; i < 256; ++i)
        perm_[256 + i] = perm_[i];
}

double Noise3::at(double x, double y, double z) const {
    const double fx = floor(x), fy = floor(y), fz = floor(z);
    const u32 xi = static_cast<u32>(static_cast<i64>(fx) & 255);
    const u32 yi = static_cast<u32>(static_cast<i64>(fy) & 255);
    const u32 zi = static_cast<u32>(static_cast<i64>(fz) & 255);
    const double xf = x - fx, yf = y - fy, zf = z - fz;
    const double u = fade(xf), v = fade(yf), w = fade(zf);
    const u32 a = perm_[xi] + yi, aa = perm_[a] + zi, ab = perm_[a + 1] + zi;
    const u32 b = perm_[xi + 1] + yi, ba = perm_[b] + zi, bb = perm_[b + 1] + zi;
    const double x1 = lerp(grad3(perm_[aa], xf, yf, zf), grad3(perm_[ba], xf - 1, yf, zf), u);
    const double x2 = lerp(grad3(perm_[ab], xf, yf - 1, zf), grad3(perm_[bb], xf - 1, yf - 1, zf), u);
    const double x3 = lerp(grad3(perm_[aa + 1], xf, yf, zf - 1), grad3(perm_[ba + 1], xf - 1, yf, zf - 1), u);
    const double x4 =
        lerp(grad3(perm_[ab + 1], xf, yf - 1, zf - 1), grad3(perm_[bb + 1], xf - 1, yf - 1, zf - 1), u);
    return lerp(lerp(x1, x2, v), lerp(x3, x4, v), w);
}

double Noise3::fbm(double x, double y, double z, u32 octaves, double freq) const {
    double sum = 0, amp = 1, norm = 0;
    const double base = freq;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * at(x * freq, y * freq, z * freq / base);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0 ? sum / norm : 0;
}

void node_field3(u64 seed, u32 field, const Array<double>& x, const Array<double>& y, double z,
                 Array<double>* out) {
    Noise3 noise;
    noise.init(seed, field);
    Array<u64> keys;
    keys.resize(x.size());
    for (usize i = 0; i < x.size(); ++i)
        keys[i] = order_key(noise.fbm(x[i], y[i], z));
    rank_normalise(keys, out);
}

void Noise2::init(u64 seed, u32 field) {
    Rng r(seed, Rng::kWorldNode, 0x6E6F697365000000ull + field); // "noise"
    for (u32 i = 0; i < 256; ++i)
        perm_[i] = static_cast<u8>(i);
    for (u32 i = 255; i > 0; --i) {
        const u32 j = static_cast<u32>(r.below(i + 1));
        const u8 t = perm_[i];
        perm_[i] = perm_[j];
        perm_[j] = t;
    }
    for (u32 i = 0; i < 256; ++i)
        perm_[256 + i] = perm_[i];
}

double Noise2::at(double x, double y) const {
    const double fx = floor(x), fy = floor(y);
    const u32 xi = static_cast<u32>(static_cast<i64>(fx) & 255);
    const u32 yi = static_cast<u32>(static_cast<i64>(fy) & 255);
    const double xf = x - fx, yf = y - fy;
    const double u = fade(xf), v = fade(yf);
    const u32 a = perm_[xi] + yi, b = perm_[xi + 1] + yi;
    const double x1 = lerp(grad(perm_[a], xf, yf), grad(perm_[b], xf - 1, yf), u);
    const double x2 = lerp(grad(perm_[a + 1], xf, yf - 1), grad(perm_[b + 1], xf - 1, yf - 1), u);
    return lerp(x1, x2, v);
}

double Noise2::fbm(double x, double y, u32 octaves, double freq) const {
    double sum = 0, amp = 1, norm = 0;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * at(x * freq, y * freq);
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return norm > 0 ? sum / norm : 0;
}

void node_field(u64 seed, u32 field, const Array<double>& x, const Array<double>& y, Array<double>* out) {
    const usize n = x.size();
    out->resize(n);
    if (n == 1) {
        (*out)[0] = 0;
        return;
    }
    Noise2 noise;
    noise.init(seed, field);
    Array<u64> keys, sorted;
    keys.resize(n);
    for (usize i = 0; i < n; ++i)
        keys[i] = order_key(noise.fbm(x[i], y[i]));
    sorted.assign(keys.data(), n);
    sort_u64(sorted.data(), n);
    for (usize i = 0; i < n; ++i)
        (*out)[i] = -1.0 + 2.0 * static_cast<double>(count_below(sorted, keys[i])) / static_cast<double>(n - 1);
}

void generate_graph(u64 seed, const GraphParams& gp, ProcGraph* g) {
    const u32 n = gp.nodes;
    Rng r(seed, Rng::kWorldNode, 0x6772617068000000ull); // "graph"

    // --- placement: jittered grid -------------------------------------------
    u32 side = static_cast<u32>(ceil(sqrt(static_cast<double>(n))));
    while (static_cast<u64>(side) * side < n)
        ++side;
    const u32 cells = side * side;
    Array<u64> pick;
    pick.resize(cells);
    for (u32 c = 0; c < cells; ++c)
        pick[c] = c;
    for (u32 i = 0; i < n && i + 1 < cells; ++i) { // partial Fisher-Yates
        const u32 j = i + static_cast<u32>(r.below(cells - i));
        const u64 t = pick[i];
        pick[i] = pick[j];
        pick[j] = t;
    }
    sort_u64(pick.data(), n); // node ids follow the cells, row-major
    g->x.resize(n);
    g->y.resize(n);
    for (u32 i = 0; i < n; ++i) {
        const u32 c = static_cast<u32>(pick[i]);
        g->x[i] = (static_cast<double>(c % side) + 0.1 + 0.8 * r.unit()) / side;
        g->y[i] = (static_cast<double>(c / side) + 0.1 + 0.8 * r.unit()) / side;
    }

    // --- proximity links, ruggedness and long links ---------------------------
    Array<u64> undirected;
    proximity(*g, gp, nullptr, &undirected);

    // Ruggedness: rank each proximity link's midpoint value; the roughest
    // fraction is cut. The threshold is kept (the key at that rank), so a
    // rebuild among live sites cuts the same way, locally.
    Noise2 rug;
    rug.init(seed, kFieldRugged);
    g->rug_sorted.resize(undirected.size());
    for (usize i = 0; i < undirected.size(); ++i)
        g->rug_sorted[i] = rug_key(rug, *g, static_cast<u32>(undirected[i] >> 32), static_cast<u32>(undirected[i]));
    sort_u64(g->rug_sorted.data(), g->rug_sorted.size());
    const usize count = g->rug_sorted.size();
    g->rug_cut = ~0ull;
    if (gp.ruggedness > 0) {
        for (usize i = 0; i < count; ++i) {
            if (static_cast<double>(i) / static_cast<double>(count) >= 1.0 - gp.ruggedness) {
                g->rug_cut = g->rug_sorted[i];
                break;
            }
        }
    }
    HashMap<u32> linked;
    g->cut = 0;
    for (usize i = 0; i < undirected.size(); ++i) {
        const u32 a = static_cast<u32>(undirected[i] >> 32), b = static_cast<u32>(undirected[i]);
        if (rug_key(rug, *g, a, b) >= g->rug_cut) {
            ++g->cut;
            continue;
        }
        linked.get_or_insert(undirected[i]) = 1;
    }

    // Long links: the rivers and trade routes.
    const u32 want = static_cast<u32>(gp.long_links * n + 0.5);
    g->long_added = 0;
    g->long_pairs.clear();
    for (u32 l = 0; l < want && n >= 3; ++l) {
        for (u32 attempt = 0; attempt < 32; ++attempt) {
            const u32 a = static_cast<u32>(r.below(n));
            u32 far = 0xFFFFFFFFu;
            double far_d = -1;
            for (u32 c = 0; c < 8; ++c) {
                const u32 b = static_cast<u32>(r.below(n));
                if (b == a)
                    continue;
                const double d = dist2(*g, a, b);
                if (d > far_d) {
                    far_d = d;
                    far = b;
                }
            }
            if (far == 0xFFFFFFFFu || linked.contains(pair_key(a, far)))
                continue;
            g->long_pairs.push(pair_key(a, far));
            linked.get_or_insert(pair_key(a, far)) = 1;
            ++g->long_added;
            break;
        }
    }

    assemble_graph(seed, gp, g, nullptr);
}

void assemble_graph(u64 seed, const GraphParams& gp, ProcGraph* g, const u8* live) {
    Noise2 rug;
    rug.init(seed, kFieldRugged);
    Array<u64> undirected, kept;
    proximity(*g, gp, live, &undirected);
    for (usize i = 0; i < undirected.size(); ++i) {
        const u32 a = static_cast<u32>(undirected[i] >> 32), b = static_cast<u32>(undirected[i]);
        if (rug_key(rug, *g, a, b) < g->rug_cut)
            kept.push(undirected[i]);
    }
    for (usize i = 0; i < g->long_pairs.size(); ++i) {
        const u32 a = static_cast<u32>(g->long_pairs[i] >> 32), b = static_cast<u32>(g->long_pairs[i]);
        if (!live || (live[a] && live[b]))
            kept.push(g->long_pairs[i]);
    }

    // Directed edges, sorted and unique, with their latency and bandwidth.
    g->keys.clear();
    for (usize i = 0; i < kept.size(); ++i) {
        const u64 a = kept[i] >> 32, b = kept[i] & 0xFFFFFFFFull;
        g->keys.push((a << 32) | b);
        g->keys.push((b << 32) | a);
    }
    sort_u64(g->keys.data(), g->keys.size());
    usize unique = 0;
    for (usize i = 0; i < g->keys.size(); ++i)
        if (unique == 0 || g->keys[i] != g->keys[unique - 1])
            g->keys[unique++] = g->keys[i];
    g->keys.resize(unique);
    const double count = static_cast<double>(g->rug_sorted.size());
    const usize e = g->keys.size();
    g->latency.resize(e);
    g->bandwidth.resize(e);
    g->dist.resize(e);
    for (usize i = 0; i < e; ++i) {
        const u32 a = static_cast<u32>(g->keys[i] >> 32), b = static_cast<u32>(g->keys[i]);
        // The midpoint and rank are symmetric, so both directions agree.
        const u32 lo = a < b ? a : b, hi = a < b ? b : a;
        const double d = sqrt(dist2(*g, lo, hi));
        const double u =
            count > 0 ? static_cast<double>(count_below(g->rug_sorted, rug_key(rug, *g, lo, hi))) / count : 0.0;
        g->dist[i] = d;
        g->latency[i] = 1 + static_cast<u32>(floor(d * gp.latency_scale));
        const double bw = floor(gp.bandwidth * (1.0 - 0.75 * (u < 1.0 ? u : 1.0)));
        g->bandwidth[i] = bw < 1.0 ? 1u : static_cast<u32>(bw);
    }
}

} // namespace nova
