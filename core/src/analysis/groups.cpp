// Group behaviour (docs/analysis-plan.md A3; docs/results/profiles.md), each
// measure against what chance would give:
//
//   moving together     same-lineage arrivals at one node within `gap` ticks
//                       of each other (arrivals.csv), against the lineage
//                       labels shuffled among the arrivals of the same node
//                       and block of ticks (which keeps the arrivals' times,
//                       sources and each block's mix of lineages);
//   kin clustering      a sampled body's nearest neighbours in memory being of
//                       its lineage (or genome), against picking a neighbour
//                       at random among the node's living bodies: chance
//                       (kin - 1) / (bodies - 1) a side, from the sample's own
//                       census (profiles.csv);
//   listening then acting  the mutual information between a LISTEN's heard
//                       value (zero, positive, negative) and which way the
//                       next conditional branch went, pooled over the samples,
//                       against the branches' ways shuffled within each sample.
//
// A measure is found when it beats chance by `found` standard deviations.
#include <cmath>  // log, sqrt
#include <cstdio> // snprintf

#include "nova/analysis.h"
#include "nova/rng.h"

namespace nova {

namespace {

void finish_measure(GroupMeasure* m, double found) {
    m->z = m->sd > 0 ? (m->observed - m->expected) / m->sd : (m->observed > m->expected ? 1e9 : 0.0);
    m->found = m->n > 0 && m->z >= found;
}

void shuffle(Array<u64>& v, usize from, usize to, Rng& r) {
    for (usize i = to; i > from + 1; --i) {
        const usize j = from + static_cast<usize>(r.below(i - from));
        const u64 t = v[i - 1];
        v[i - 1] = v[j];
        v[j] = t;
    }
}

// Mutual information (bits) of a 3 x 2 table.
double mutual_info(const u32* t) {
    double n = 0;
    for (u32 i = 0; i < 6; ++i)
        n += t[i];
    if (n <= 0)
        return 0;
    double row[3], col[2] = {0, 0};
    for (u32 i = 0; i < 3; ++i) {
        row[i] = t[2 * i] + t[2 * i + 1];
        col[0] += t[2 * i];
        col[1] += t[2 * i + 1];
    }
    double mi = 0;
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 2; ++j) {
            const double c = t[2 * i + j];
            if (c > 0)
                mi += c / n * log(c * n / (row[i] * col[j])) / log(2.0);
        }
    return mi < 0 ? 0 : mi;
}

} // namespace

void moving_together(const Array<ArrivalRecord>& a, const GroupOptions& o, GroupMeasure* all, GroupMeasure* apart) {
    *all = GroupMeasure{};
    *apart = GroupMeasure{};
    if (a.size() < 2)
        return;
    const u64 block = o.block ? o.block : 1;
    // Group by node, keeping each node's arrivals in file order (the log is in
    // tick order): a counting sort.
    u32 nodes = 0;
    for (usize i = 0; i < a.size(); ++i)
        nodes = a[i].node + 1 > nodes ? a[i].node + 1 : nodes;
    Array<u32> start;
    start.resize(static_cast<usize>(nodes) + 1);
    for (usize i = 0; i < a.size(); ++i)
        ++start[a[i].node + 1];
    for (u32 n = 0; n < nodes; ++n)
        start[n + 1] += start[n];
    Array<u32> at, fill;
    at.resize(a.size());
    fill.assign(start.data(), start.size());
    for (usize i = 0; i < a.size(); ++i)
        at[fill[a[i].node]++] = static_cast<u32>(i);
    // Groups: runs of one node and one block of ticks. Pairs: arrivals in a
    // group at most `gap` ticks apart.
    Array<u64> label;   // per place in `at`: the lineage (shuffled within groups)
    Array<u32> gstart;  // group boundaries in `at`
    Array<u32> pi, pj;  // the pairs (places in `at`)
    Array<u8> diff_src; // ... whether their sources differ
    label.resize(a.size());
    for (usize p = 0; p < at.size(); ++p)
        label[p] = a[at[p]].lineage;
    constexpr usize kMaxPairs = 20000000;
    for (usize p = 0; p < at.size(); ++p) {
        const ArrivalRecord& x = a[at[p]];
        if (p == 0 || a[at[p - 1]].node != x.node || a[at[p - 1]].tick / block != x.tick / block)
            gstart.push(static_cast<u32>(p));
        for (usize q = p + 1; q < at.size() && pi.size() < kMaxPairs; ++q) {
            const ArrivalRecord& y = a[at[q]];
            if (y.node != x.node || y.tick / block != x.tick / block || y.tick - x.tick > o.gap)
                break;
            pi.push(static_cast<u32>(p));
            pj.push(static_cast<u32>(q));
            diff_src.push(x.src != y.src ? 1 : 0);
        }
    }
    gstart.push(static_cast<u32>(at.size()));
    auto count = [&](const Array<u64>& lab, u64* same, u64* same_apart) {
        *same = *same_apart = 0;
        for (usize k = 0; k < pi.size(); ++k) {
            const u64 l = lab[pi[k]];
            if (l != 0 && l == lab[pj[k]]) {
                ++*same;
                *same_apart += diff_src[k];
            }
        }
    };
    u64 s = 0, sa = 0;
    count(label, &s, &sa);
    all->observed = static_cast<double>(s);
    apart->observed = static_cast<double>(sa);
    all->n = pi.size();
    u64 napart = 0;
    for (usize k = 0; k < diff_src.size(); ++k)
        napart += diff_src[k];
    apart->n = napart;
    Rng r = Rng::from_base(o.seed ^ 0x67726F7570730000ull); // "groups"
    Array<u64> lab;
    double m1 = 0, m2 = 0, a1 = 0, a2 = 0;
    const u32 R = o.shuffles ? o.shuffles : 1;
    for (u32 it = 0; it < R; ++it) {
        lab.assign(label.data(), label.size());
        for (usize g = 0; g + 1 < gstart.size(); ++g)
            shuffle(lab, gstart[g], gstart[g + 1], r);
        count(lab, &s, &sa);
        m1 += static_cast<double>(s);
        m2 += static_cast<double>(s) * static_cast<double>(s);
        a1 += static_cast<double>(sa);
        a2 += static_cast<double>(sa) * static_cast<double>(sa);
    }
    all->expected = m1 / R;
    all->sd = sqrt(m2 / R - all->expected * all->expected > 0 ? m2 / R - all->expected * all->expected : 0);
    apart->expected = a1 / R;
    apart->sd = sqrt(a2 / R - apart->expected * apart->expected > 0 ? a2 / R - apart->expected * apart->expected : 0);
    finish_measure(all, o.found);
    finish_measure(apart, o.found);
}

void kin_clustering(const Array<ProfileRow>& rows, const GroupOptions& o, GroupMeasure* lineage,
                    GroupMeasure* genotype) {
    *lineage = GroupMeasure{};
    *genotype = GroupMeasure{};
    double vl = 0, vg = 0;
    for (usize i = 0; i < rows.size(); ++i) {
        const ProfileRow& r = rows[i];
        if (r.node_bodies < 2)
            continue;
        const double others = static_cast<double>(r.node_bodies - 1);
        const u32 sides[2] = {r.nb_left, r.nb_right};
        for (u32 side : sides) {
            if (!(side & kNbFound))
                continue;
            if (r.lineage != 0 && r.lin_alive >= 1) {
                const double p = (r.lin_alive - 1) / others;
                lineage->observed += (side & kNbLineage) ? 1.0 : 0.0;
                lineage->expected += p;
                vl += p * (1 - p);
                ++lineage->n;
            }
            if (r.geno_alive >= 1) {
                const double p = (r.geno_alive - 1) / others;
                genotype->observed += (side & kNbGenotype) ? 1.0 : 0.0;
                genotype->expected += p;
                vg += p * (1 - p);
                ++genotype->n;
            }
        }
    }
    lineage->sd = sqrt(vl);
    genotype->sd = sqrt(vg);
    finish_measure(lineage, o.found);
    finish_measure(genotype, o.found);
}

void listening_acting(const Array<ProfileRow>& rows, const GroupOptions& o, GroupMeasure* out) {
    *out = GroupMeasure{};
    constexpr u32 kMinPairs = 4, kCap = 4096;
    u32 R = o.shuffles ? o.shuffles : 1;
    // The samples tested (at least kMinPairs listen-branch pairs), each table
    // scaled down to at most kCap pairs.
    Array<u32> tables; // 6 a sample
    u64 work = 0;
    for (usize i = 0; i < rows.size(); ++i) {
        const u32* t = rows[i].lta;
        u64 m = 0;
        for (u32 j = 0; j < 6; ++j)
            m += t[j];
        if (m < kMinPairs)
            continue;
        u32 s[6];
        for (u32 j = 0; j < 6; ++j)
            s[j] = m > kCap ? static_cast<u32>(static_cast<u64>(t[j]) * kCap / m) : t[j];
        tables.append(s, 6);
        work += m > kCap ? kCap : m;
    }
    const usize ns = tables.size() / 6;
    out->n = ns;
    if (ns == 0)
        return;
    if (work * R > 200000000ull) // keep it to a few seconds
        R = static_cast<u32>(200000000ull / work) > 50 ? static_cast<u32>(200000000ull / work) : 50;
    Rng r = Rng::from_base(o.seed ^ 0x6C697374656E0000ull); // "listen"
    Array<double> tot;
    tot.resize(R);
    Array<u8> cls, way;
    for (usize k = 0; k < ns; ++k) {
        const u32* t = tables.data() + 6 * k;
        const double mi = mutual_info(t);
        out->observed += mi;
        cls.clear();
        way.clear();
        for (u32 c = 0; c < 3; ++c)
            for (u32 w = 0; w < 2; ++w)
                for (u32 j = 0; j < t[2 * c + w]; ++j) {
                    cls.push(static_cast<u8>(c));
                    way.push(static_cast<u8>(w));
                }
        u32 ge = 0;
        for (u32 it = 0; it < R; ++it) {
            for (usize i = way.size(); i > 1; --i) {
                const usize j = static_cast<usize>(r.below(i));
                const u8 v = way[i - 1];
                way[i - 1] = way[j];
                way[j] = v;
            }
            u32 s[6] = {};
            for (usize i = 0; i < way.size(); ++i)
                ++s[cls[i] * 2 + way[i]];
            const double ms = mutual_info(s);
            tot[it] += ms;
            ge += ms >= mi - 1e-12 ? 1u : 0u;
        }
        if ((ge + 1.0) / (R + 1.0) < 0.05)
            ++out->extra;
    }
    double m1 = 0, m2 = 0;
    for (u32 it = 0; it < R; ++it) {
        m1 += tot[it];
        m2 += tot[it] * tot[it];
    }
    out->expected = m1 / R;
    const double var = m2 / R - out->expected * out->expected;
    out->sd = sqrt(var > 0 ? var : 0);
    finish_measure(out, o.found);
}

bool groups_analyse(const char* dir, const GroupOptions& o, GroupsReport* out, TextBuf* err) {
    *out = GroupsReport{};
    Array<ProfileRow> rows;
    if (!load_profiles(dir, &rows, err))
        return false;
    out->samples = rows.size();
    kin_clustering(rows, o, &out->kin_lineage, &out->kin_genotype);
    listening_acting(rows, o, &out->listening);
    Array<ArrivalRecord> arr;
    TextBuf why;
    out->have_arrivals = load_arrivals(dir, &arr, &why);
    if (!out->have_arrivals && why.size() && !Str(why.c_str()).starts_with("no arrivals.csv")) {
        err->append(why.view());
        return false;
    }
    out->arrivals = arr.size();
    moving_together(arr, o, &out->together, &out->apart);
    return true;
}

namespace {

void measure_line(TextBuf& out, const char* name, const GroupMeasure& m, const char* unit) {
    char line[400];
    snprintf(line, sizeof line, "  %-28s observed %.4g, chance %.4g +- %.3g (%s %.2f), z = %.2f over %llu %s: %s\n",
             name, m.observed, m.expected, m.sd, "ratio", m.expected > 0 ? m.observed / m.expected : 0.0,
             m.z > 1e8 ? 99.99 : m.z, static_cast<unsigned long long>(m.n), unit, m.found ? "FOUND" : "none");
    out.append(line);
}

} // namespace

void groups_summary(const GroupsReport& r, const GroupOptions& o, TextBuf& out) {
    char line[300];
    snprintf(line, sizeof line, "groups: %llu samples, %llu arrivals%s; found at z >= %.1f\n",
             static_cast<unsigned long long>(r.samples), static_cast<unsigned long long>(r.arrivals),
             r.have_arrivals ? "" : " (no arrivals.csv)", o.found);
    out.append(line);
    snprintf(line, sizeof line, "moving together (same-lineage arrival pairs within %u ticks; labels shuffled in "
                                "%u-tick blocks per node, %u times):\n",
             o.gap, o.block, o.shuffles);
    out.append(line);
    measure_line(out, "all pairs", r.together, "pairs");
    measure_line(out, "from different neighbours", r.apart, "pairs");
    out.append("kin clustering (a sampled body's memory neighbours; chance = a random other living body):\n");
    measure_line(out, "same lineage", r.kin_lineage, "sides");
    measure_line(out, "same genome", r.kin_genotype, "sides");
    out.append("listening then acting (heard value vs the next branch's way; mutual information, bits, summed):\n");
    measure_line(out, "pooled", r.listening, "samples");
    snprintf(line, sizeof line, "  %-28s %llu of %llu samples (%.1f%%; chance at most 5%%)\n",
             "individually p < 0.05", static_cast<unsigned long long>(r.listening.extra),
             static_cast<unsigned long long>(r.listening.n),
             r.listening.n ? 100.0 * r.listening.extra / static_cast<double>(r.listening.n) : 0.0);
    out.append(line);
}

void groups_csv(const GroupsReport& r, TextBuf& out) {
    out.append("measure,observed,expected,sd,ratio,z,n,found\n");
    const struct {
        const char* name;
        const GroupMeasure* m;
    } rows[] = {{"moving_together", &r.together},
                {"moving_together_apart", &r.apart},
                {"kin_lineage", &r.kin_lineage},
                {"kin_genotype", &r.kin_genotype},
                {"listening_acting", &r.listening}};
    for (const auto& x : rows) {
        out.append(x.name);
        const double v[] = {x.m->observed, x.m->expected, x.m->sd,
                            x.m->expected > 0 ? x.m->observed / x.m->expected : 0.0, x.m->z > 1e8 ? 99.99 : x.m->z};
        for (double d : v) {
            out.append_char(',');
            out.append_f64(d);
        }
        out.append_char(',');
        out.append_u64(x.m->n);
        out.append(x.m->found ? ",1\n" : ",0\n");
    }
}

} // namespace nova
