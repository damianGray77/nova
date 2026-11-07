// Neighbour joining, Newick text and path lengths (docs/analysis-plan.md A0,
// for A4).
#include <cstdio> // snprintf

#include "nova/learn.h"

namespace nova {

namespace {

i32 add_node(Tree* t) {
    TreeNode nd;
    t->nodes.push(nd);
    return static_cast<i32>(t->nodes.size() - 1);
}

void attach(Tree* t, i32 parent, i32 child, double length) {
    TreeNode& p = t->nodes[static_cast<usize>(parent)];
    NOVA_ASSERT(p.children < 3);
    p.child[p.children++] = child;
    TreeNode& c = t->nodes[static_cast<usize>(child)];
    c.parent = parent;
    c.length = length;
}

// Splits a joined length so neither branch is negative.
void split(double total, double* a, double* b) {
    if (*a < 0) {
        *a = 0;
        *b = total;
    } else if (*b < 0) {
        *b = 0;
        *a = total;
    }
    if (*a < 0)
        *a = 0;
    if (*b < 0)
        *b = 0;
}

} // namespace

void neighbour_join(const Matrix& d, Tree* out) {
    const usize n = d.rows;
    NOVA_ASSERT(d.cols == n);
    out->nodes.clear();
    out->leaves = static_cast<u32>(n);
    out->root = -1;
    for (usize i = 0; i < n; ++i)
        add_node(out);
    if (n == 0)
        return;
    if (n == 1) {
        out->root = 0;
        return;
    }
    // Working distances between the active slots; slot s holds node id[s].
    Matrix w;
    w.resize(n, n);
    memcpy(w.v.data(), d.v.data(), n * n * sizeof(double));
    Array<i32> id;
    Array<u8> live;
    Array<double> r;
    id.resize(n);
    live.resize(n);
    r.resize(n);
    for (usize i = 0; i < n; ++i) {
        id[i] = static_cast<i32>(i);
        live[i] = 1;
    }
    double* W = w.v.data();
    // The active slots, in increasing order (so ties go to the lowest pair),
    // and each one's row sum over the others.
    Array<usize> act;
    act.resize(n);
    for (usize i = 0; i < n; ++i)
        act[i] = i;
    for (usize i = 0; i < n; ++i) {
        double s = 0;
        for (usize k = 0; k < n; ++k)
            if (k != i)
                s += W[i * n + k];
        r[i] = s;
    }
    usize m = n;
    while (m > 3) {
        usize bi = 0, bj = 0, bpos = 0;
        double bq = 0;
        bool have = false;
        const double m2 = static_cast<double>(m - 2);
        for (usize a = 0; a < m; ++a) {
            const usize i = act[a];
            const double* Wi = W + i * n;
            const double ri = r[i];
            for (usize b = a + 1; b < m; ++b) {
                const usize j = act[b];
                const double q = m2 * Wi[j] - ri - r[j];
                if (!have || q < bq) {
                    have = true;
                    bq = q;
                    bi = i;
                    bj = j;
                    bpos = b;
                }
            }
        }
        const double dij = W[bi * n + bj];
        double li = 0.5 * dij + (r[bi] - r[bj]) / (2.0 * m2);
        double lj = dij - li;
        split(dij, &li, &lj);
        const i32 u = add_node(out);
        attach(out, u, id[bi], li);
        attach(out, u, id[bj], lj);
        // The new node takes slot bi; bj leaves. Row sums follow.
        double ru = 0;
        for (usize a = 0; a < m; ++a) {
            const usize k = act[a];
            if (k == bi || k == bj)
                continue;
            const double duk = 0.5 * (W[bi * n + k] + W[bj * n + k] - dij);
            r[k] += duk - W[bi * n + k] - W[bj * n + k];
            W[bi * n + k] = duk;
            W[k * n + bi] = duk;
            ru += duk;
        }
        W[bi * n + bi] = 0;
        r[bi] = ru;
        id[bi] = u;
        live[bj] = 0;
        for (usize a = bpos; a + 1 < m; ++a)
            act[a] = act[a + 1];
        --m;
    }
    // The last two or three meet at the root.
    usize s[3] = {0, 0, 0};
    usize c = 0;
    for (usize i = 0; i < n && c < 3; ++i)
        if (live[i])
            s[c++] = i;
    const i32 root = add_node(out);
    if (c == 2) {
        const double dab = W[s[0] * n + s[1]];
        attach(out, root, id[s[0]], 0.5 * dab);
        attach(out, root, id[s[1]], 0.5 * dab);
    } else {
        const double dab = W[s[0] * n + s[1]], dac = W[s[0] * n + s[2]], dbc = W[s[1] * n + s[2]];
        double la = 0.5 * (dab + dac - dbc), lb = 0.5 * (dab + dbc - dac), lc = 0.5 * (dac + dbc - dab);
        if (la < 0) {
            lb += la;
            lc += la;
            la = 0;
        }
        if (lb < 0) {
            la += lb;
            lc += lb;
            lb = 0;
        }
        if (lc < 0) {
            la += lc;
            lb += lc;
            lc = 0;
        }
        attach(out, root, id[s[0]], la < 0 ? 0 : la);
        attach(out, root, id[s[1]], lb < 0 ? 0 : lb);
        attach(out, root, id[s[2]], lc < 0 ? 0 : lc);
    }
    out->root = root;
}

void tree_newick(const Tree& t, const Str* labels, TextBuf& out) {
    if (t.root < 0) {
        out.append(";");
        return;
    }
    // Iterative depth-first walk: (node, next child to visit).
    struct Frame {
        i32 node;
        u32 next;
    };
    Array<Frame> stack;
    stack.push(Frame{t.root, 0});
    char num[32];
    while (!stack.empty()) {
        Frame& f = stack.back();
        const TreeNode& nd = t.nodes[static_cast<usize>(f.node)];
        if (nd.children == 0) { // a leaf
            out.append(labels[f.node]);
        } else if (f.next < nd.children) {
            out.append_char(f.next == 0 ? '(' : ',');
            const i32 c = nd.child[f.next++];
            stack.push(Frame{c, 0});
            continue;
        } else {
            out.append_char(')');
        }
        const i32 done = f.node;
        stack.pop();
        if (done != t.root) {
            snprintf(num, sizeof num, ":%.6g", t.nodes[static_cast<usize>(done)].length);
            out.append(num);
        }
    }
    out.append(";");
}

void tree_distances(const Tree& t, u32 from, double* dist) {
    const usize nn = t.nodes.size();
    for (usize i = 0; i < nn; ++i)
        dist[i] = -1;
    Array<i32> stack;
    dist[from] = 0;
    stack.push(static_cast<i32>(from));
    while (!stack.empty()) {
        const i32 v = stack.back();
        stack.pop();
        const TreeNode& nd = t.nodes[static_cast<usize>(v)];
        const double dv = dist[v];
        if (nd.parent >= 0 && dist[nd.parent] < 0) {
            dist[nd.parent] = dv + nd.length;
            stack.push(nd.parent);
        }
        for (u32 k = 0; k < nd.children; ++k) {
            const i32 c = nd.child[k];
            if (dist[c] < 0) {
                dist[c] = dv + t.nodes[static_cast<usize>(c)].length;
                stack.push(c);
            }
        }
    }
}

} // namespace nova
