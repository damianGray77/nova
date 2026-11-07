// Sequence alignment that tolerates shifts and rotations (docs/analysis-plan.md
// A0, for A4).
#include "nova/learn.h"

namespace nova {

namespace {

u32 absdiff(u32 a, u32 b) {
    return a > b ? a - b : b - a;
}

// The pattern p (length n) aligned against the text t written twice (length
// 2m), free to start and end anywhere in it; the part of t's one copy that the
// matched stretch misses or repeats is charged too (|m - span|). Unit costs.
Alignment semi_global_cyclic(const u8* p, u32 n, const u8* t, u32 m, AlignScratch& s) {
    Alignment best;
    if (m == 0) {
        best.cost = n;
        return best;
    }
    const u32 w = 2 * m;
    s.text.resize(w);
    u8* tt = s.text.data();
    memcpy(tt, t, m);
    memcpy(tt + m, t, m);
    s.cost0.resize(w + 1);
    s.cost1.resize(w + 1);
    s.start0.resize(w + 1);
    s.start1.resize(w + 1);
    u32* pc = s.cost0.data(); // the previous row
    u32* cc = s.cost1.data(); // this row
    u32* ps = s.start0.data();
    u32* cs = s.start1.data();
    for (u32 j = 0; j <= w; ++j) {
        pc[j] = 0; // a free start anywhere
        ps[j] = j;
    }
    for (u32 i = 1; i <= n; ++i) {
        cc[0] = i;
        cs[0] = 0;
        const u8 a = p[i - 1];
        for (u32 j = 1; j <= w; ++j) {
            // Ties prefer the diagonal, then the pattern gap, then the text gap.
            u32 c = pc[j - 1] + (a != tt[j - 1] ? 1u : 0u);
            u32 st = ps[j - 1];
            const u32 up = pc[j] + 1;
            if (up < c) {
                c = up;
                st = ps[j];
            }
            const u32 left = cc[j - 1] + 1;
            if (left < c) {
                c = left;
                st = cs[j - 1];
            }
            cc[j] = c;
            cs[j] = st;
        }
        u32* t1 = pc;
        pc = cc;
        cc = t1;
        t1 = ps;
        ps = cs;
        cs = t1;
    }
    // pc / ps now hold the last row.
    best.cost = 0xFFFFFFFFu;
    for (u32 j = 1; j <= w; ++j) {
        const u32 span = j - ps[j];
        const u32 c = pc[j] + absdiff(m, span);
        if (c < best.cost) {
            best.cost = c;
            best.offset = ps[j] % m;
        }
    }
    if (n == 0) {
        best.cost = m;
        best.offset = 0;
    }
    return best;
}

} // namespace

Alignment align_cyclic(const u8* a, u32 n, const u8* b, u32 m, AlignScratch* scratch) {
    AlignScratch local;
    AlignScratch& s = scratch ? *scratch : local;
    Alignment r;
    if (n < m) {
        r = semi_global_cyclic(a, n, b, m, s);
    } else if (m < n) {
        r = semi_global_cyclic(b, m, a, n, s);
    } else {
        r = semi_global_cyclic(a, n, b, m, s);
        const Alignment q = semi_global_cyclic(b, m, a, n, s);
        if (q.cost < r.cost) {
            r = q;
            r.offset = q.offset ? m - q.offset : 0; // as a rotation of b
        }
    }
    const u32 longer = n > m ? n : m;
    r.distance = longer ? static_cast<double>(r.cost) / longer : 0.0;
    return r;
}

u32 edit_distance(const u8* a, u32 n, const u8* b, u32 m) {
    Array<u32> prev, cur;
    prev.resize(m + 1);
    cur.resize(m + 1);
    for (u32 j = 0; j <= m; ++j)
        prev[j] = j;
    for (u32 i = 1; i <= n; ++i) {
        cur[0] = i;
        for (u32 j = 1; j <= m; ++j) {
            u32 c = prev[j - 1] + (a[i - 1] != b[j - 1] ? 1u : 0u);
            if (prev[j] + 1 < c)
                c = prev[j] + 1;
            if (cur[j - 1] + 1 < c)
                c = cur[j - 1] + 1;
            cur[j] = c;
        }
        for (u32 j = 0; j <= m; ++j)
            prev[j] = cur[j];
    }
    return prev[m];
}

double same_offset_similarity(const u8* a, u32 n, const u8* b, u32 m) {
    const u32 shorter = n < m ? n : m, longer = n > m ? n : m;
    if (longer == 0)
        return 1.0;
    u32 same = 0;
    for (u32 i = 0; i < shorter; ++i)
        same += a[i] == b[i];
    return static_cast<double>(same) / longer;
}

} // namespace nova
