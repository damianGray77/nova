// Family trees from genomes (docs/analysis-plan.md A4): alignment distances
// that forgive rotations and shifts, neighbour joining, the tracker's parent
// links checked against the tree, and founders linked to a likely ancestor.
#include <cstdio> // snprintf

#include "nova/analysis.h"
#include "nova/pool.h"
#include "nova/rng.h"

namespace nova {

namespace {

void label_of(const TreeGenome& g, TextBuf& out) {
    out.append(g.name);
    out.append("_n");
    out.append_u64(g.node);
}

struct DistJob {
    const TreeInput* in;
    Matrix* d;
};

void dist_row(void* ctx, u32 i) {
    const DistJob& j = *static_cast<const DistJob*>(ctx);
    const Array<TreeGenome>& L = j.in->leaves;
    const u8* sym = j.in->sym.data();
    AlignScratch s;
    const usize n = L.size();
    double* row = j.d->row(i);
    for (usize k = i + 1; k < n; ++k)
        row[k] = align_cyclic(sym + L[i].off, L[i].len, sym + L[k].off, L[k].len, &s).distance;
}

double median_of(Array<double>& v) {
    if (v.empty())
        return 0;
    Array<u64> keys;
    keys.resize(v.size());
    for (usize i = 0; i < v.size(); ++i) {
        u64 b;
        memcpy(&b, &v[i], 8); // non-negative doubles sort as their bits
        keys[i] = b;
    }
    sort_u64(keys.data(), keys.size());
    double m;
    memcpy(&m, &keys[keys.size() / 2], 8);
    return m;
}

void csv_num(TextBuf& out, double v) {
    char num[32];
    snprintf(num, sizeof num, "%.4f", v);
    out.append(num);
}

} // namespace

bool tree_build(const TreeInput& in, const TreeOptions& o, TreeReport* out, TextBuf* err) {
    const usize n = in.leaves.size();
    if (n < 2) {
        err->append("a tree needs at least 2 genotypes");
        return false;
    }
    if (n > kTreeMaxLeaves) {
        err->append("too many genotypes for one tree (neighbour joining grows as the cube; at most 4000)");
        return false;
    }
    out->leaves = static_cast<u32>(n);
    const u8* sym = in.sym.data();

    // Distances.
    Matrix d;
    d.resize(n, n);
    DistJob job{&in, &d};
    if (o.threads > 1) {
        ThreadPool pool;
        pool.start(o.threads);
        pool.run(static_cast<u32>(n), dist_row, &job);
    } else {
        for (u32 i = 0; i < n; ++i)
            dist_row(&job, i);
    }
    for (usize i = 0; i < n; ++i)
        for (usize k = i + 1; k < n; ++k)
            d.at(k, i) = d.at(i, k);

    // The tree.
    Tree t;
    neighbour_join(d, &t);
    TextBuf names;
    Array<u32> name_off, name_len;
    for (usize i = 0; i < n; ++i) {
        name_off.push(static_cast<u32>(names.size()));
        label_of(in.leaves[i], names);
        name_len.push(static_cast<u32>(names.size()) - name_off.back());
    }
    Array<Str> labels;
    labels.resize(t.nodes.size());
    for (usize i = 0; i < n; ++i)
        labels[i] = Str(names.view().p + name_off[i], name_len[i]);
    out->newick.clear();
    tree_newick(t, labels.data(), out->newick);
    out->newick.append_char('\n');

    HashMap<u32> by_hash;
    for (usize i = 0; i < n; ++i)
        by_hash.get_or_insert(in.leaves[i].hash) = static_cast<u32>(i);

    out->links = out->agree = out->agree3 = out->aligned_agree = 0;
    out->founders = out->linked = 0;
    double conf_sum = 0;
    TextBuf& csv = out->links_csv;
    csv.clear();
    csv.append("genotype,kind,node,lineage,tick,len,hash,relation,tracker_parent,parent_distance,tree_nearest,"
               "parent_rank,aligned_rank,agrees,ancestor,ancestor_lineage,ancestor_distance,confidence\n");
    Array<double> dist, cand;
    dist.resize(t.nodes.size());
    AlignScratch scratch;
    TextBuf v;
    auto field = [&](Str s) {
        csv.append_char(',');
        csv.append(s);
    };
    auto field_u = [&](u64 x) {
        csv.append_char(',');
        csv.append_u64(x);
    };
    auto field_f = [&](double x) {
        csv.append_char(',');
        csv_num(csv, x);
    };
    auto empty = [&](u32 k) {
        for (u32 i = 0; i < k; ++i)
            csv.append_char(',');
    };
    for (usize c = 0; c < n; ++c) {
        const TreeGenome& g = in.leaves[c];
        tree_distances(t, static_cast<u32>(c), dist.data());
        usize nearest = c == 0 ? 1 : 0;
        for (usize k = 0; k < n; ++k)
            if (k != c && dist[k] < dist[nearest])
                nearest = k;
        csv.append(labels[c]);
        field(g.kind == kLeafNeighbour ? "neighbour" : "member");
        field_u(g.node);
        v.clear();
        append_lineage_id(v, g.lineage);
        field(v.view());
        field_u(g.tick);
        field_u(g.len);
        v.clear();
        v.append_hex64(g.hash);
        field(v.view());
        const u32* p = g.parent_hash ? by_hash.find(g.parent_hash) : nullptr;
        if (p && *p != c) {
            // A tracker link with both ends here: does the tree put the parent
            // nearest? (Ranks count the leaves strictly nearer than it.)
            const double dp = dist[*p], ap = d.at(c, *p);
            u32 rank = 0, arank = 0;
            for (usize k = 0; k < n; ++k) {
                if (k == c || k == *p)
                    continue;
                rank += dist[k] < dp - 1e-12;
                arank += d.at(c, k) < ap - 1e-12;
            }
            ++out->links;
            out->agree += rank == 0;
            out->agree3 += rank < 3;
            out->aligned_agree += arank == 0;
            out->parent_distance += ap;
            field("link");
            field(labels[*p]);
            field_f(ap);
            field(labels[nearest]);
            field_u(rank);
            field_u(arank);
            field(rank == 0 ? "1" : "0");
            empty(4);
            csv.append_char('\n');
            continue;
        }
        const bool arrival = static_cast<u32>(g.lineage >> 32) != g.node;
        if (g.parent_hash || g.kind != kLeafMember) {
            // Its parent isn't in the tree (unsaved, or left out), or it is a
            // context genotype with no parent.
            field(g.parent_hash ? "outside" : (arrival ? "arrival" : "founder"));
            v.clear();
            if (g.parent_hash)
                v.append_hex64(g.parent_hash);
            else
                v.append("none");
            field(v.view());
            empty(1);
            field(labels[nearest]);
            empty(7);
            csv.append_char('\n');
            continue;
        }
        // No parent: the lineage's founder on its founding node (its likely
        // ancestor is an earlier genotype of another lineage), or an arrival
        // on another node, a colonist whose parent that node never saw (its
        // ancestor may be its own lineage's, elsewhere).
        ++out->founders;
        cand.clear();
        double best = 2;
        const TreeGenome* bg = nullptr;
        for (usize k = 0; k < in.pool.size(); ++k) {
            const TreeGenome& q = in.pool[k];
            if (q.tick > g.tick || (!arrival && q.lineage == g.lineage))
                continue;
            const double dq = align_cyclic(sym + g.off, g.len, sym + q.off, q.len, &scratch).distance;
            cand.push(dq);
            if (dq < best) {
                best = dq;
                bg = &q;
            }
        }
        for (usize k = 0; k < n; ++k) {
            const TreeGenome& q = in.leaves[k];
            if (k == c || q.tick > g.tick || (!arrival && q.lineage == g.lineage))
                continue;
            const double dq = d.at(c, k);
            cand.push(dq);
            if (dq < best) {
                best = dq;
                bg = &q;
            }
        }
        field(arrival ? "arrival" : "founder");
        field("none");
        empty(1);
        field(labels[nearest]);
        empty(3);
        if (!bg) {
            empty(4);
            csv.append_char('\n');
            continue;
        }
        // Confidence: how far below a typical earlier genotype's distance the
        // best one sits (0 = no closer than the median, 1 = identical).
        const double med = median_of(cand);
        const double conf = med > 0 && best < med ? (med - best) / med : 0.0;
        ++out->linked;
        conf_sum += conf;
        v.clear();
        label_of(*bg, v);
        field(v.view());
        v.clear();
        append_lineage_id(v, bg->lineage);
        field(v.view());
        field_f(best);
        field_f(conf);
        csv.append_char('\n');
    }
    out->mean_confidence = out->linked ? conf_sum / out->linked : 0.0;
    out->parent_distance = out->links ? out->parent_distance / out->links : 0.0;
    return true;
}

namespace {

// The newest `cap` genotypes seen, with their symbols.
struct Recent {
    Array<TreeGenome> g;
    Array<u8> sym;
    usize head = 0;
    usize cap = 0;

    void push(const TreeGenome& e, const u8* s) {
        TreeGenome x = e;
        x.off = static_cast<u32>(sym.size());
        sym.append(s, e.len);
        g.push(x);
        if (g.size() - head > cap)
            ++head;
        if (head > cap && head > 1024) { // compact
            Array<TreeGenome> g2;
            Array<u8> s2;
            for (usize i = head; i < g.size(); ++i) {
                TreeGenome y = g[i];
                y.off = static_cast<u32>(s2.size());
                s2.append(sym.data() + g[i].off, g[i].len);
                g2.push(y);
            }
            g = nova::move(g2);
            sym = nova::move(s2);
            head = 0;
        }
    }
};

void fill_genome(TreeGenome* g, const BankEntry& e, u32 len, u32 kind) {
    *g = TreeGenome{};
    g->hash = e.hash;
    g->parent_hash = e.parent_hash;
    g->lineage = e.lineage;
    g->tick = e.tick;
    g->node = e.node;
    g->len = len;
    g->kind = kind;
    const usize nn = e.name.n < sizeof g->name - 1 ? e.name.n : sizeof g->name - 1;
    memcpy(g->name, e.name.p, nn);
    g->name[nn] = '\0';
}

void add_to(Array<TreeGenome>& dst, Array<u8>& sym, const TreeGenome& g, const u8* s) {
    TreeGenome x = g;
    x.off = static_cast<u32>(sym.size());
    sym.append(s, g.len);
    dst.push(x);
}

// Keeps `keep` of v's entries spread evenly by position (the first always).
void thin(Array<TreeGenome>& v, usize keep) {
    if (v.size() <= keep || keep == 0)
        return;
    Array<TreeGenome> out;
    const usize n = v.size();
    for (usize i = 0; i < keep; ++i)
        out.push(v[i * n / keep]);
    v = nova::move(out);
}

// Keeps `keep` of v's entries as families, so tracker links survive: the
// first, half the budget spread evenly by position, each one's parent where
// it is among them, then more spread evenly. Save order is kept.
void thin_families(Array<TreeGenome>& v, usize keep) {
    const usize n = v.size();
    if (n <= keep || keep < 2)
        return thin(v, keep);
    HashMap<u32> at;
    for (usize i = 0; i < n; ++i)
        at.get_or_insert(v[i].hash) = static_cast<u32>(i);
    Array<u8> flag;
    flag.resize(n);
    usize count = 0;
    auto take = [&](usize j) {
        if (!flag[j] && count < keep) {
            flag[j] = 1;
            ++count;
        }
    };
    take(0);
    const usize half = keep / 2;
    for (usize i = 0; i < half; ++i)
        take(i * n / half);
    for (usize j = 0; j < n && count < keep; ++j) {
        if (!flag[j])
            continue;
        const u32* p = v[j].parent_hash ? at.find(v[j].parent_hash) : nullptr;
        if (p)
            take(*p);
    }
    for (usize i = 0; i < keep && count < keep; ++i)
        take((i * n + n / 2) / keep);
    for (usize j = 0; j < n && count < keep; ++j)
        take(j);
    Array<TreeGenome> out;
    for (usize j = 0; j < n; ++j)
        if (flag[j])
            out.push(v[j]);
    v = nova::move(out);
}

bool open_bank(const char* dir, GenebankReader* bank, OpAlphabet* abc, TextBuf* err) {
    Config cfg;
    TextBuf cerr;
    if (!load_run_config(dir, &cfg, &cerr))
        cfg.clear();
    CodonTable table;
    if (!run_codon_table(cfg, &table, err))
        return false;
    abc->init(table);
    TextBuf path;
    path_join(path, dir, "genebank.txt");
    if (!bank->open(path.c_str())) {
        err->append("cannot read '");
        err->append(path.view());
        err->append_char('\'');
        return false;
    }
    return true;
}

} // namespace

bool tree_analyse(const char* dir, const TreeOptions& o, TreeReport* out, TextBuf* err) {
    const usize cap = o.max < 2 ? 2 : (o.max > kTreeMaxLeaves ? kTreeMaxLeaves : o.max);
    GenebankReader bank;
    OpAlphabet abc;
    if (!open_bank(dir, &bank, &abc, err))
        return false;
    TreeInput in;
    HashMap<u8> seen;
    BankEntry e;
    Array<u8> s;
    TreeGenome g;
    out->candidates = 0;
    out->capped = false;

    if (o.by_lineage) {
        Recent before;
        before.cap = o.pool;
        Array<TreeGenome> members;
        Array<u8> msym;
        HashMap<u8> seen_member; // a genotype can turn up in another lineage too
        while (bank.next(&e)) {
            const bool mine = e.lineage == o.lineage;
            if (!mine && (!members.empty() || !o.pool))
                continue;
            bool fresh = false;
            (mine ? seen_member : seen).get_or_insert(e.hash, &fresh);
            if (!fresh || !abc.encode(e.genome, &s) || s.empty())
                continue;
            fill_genome(&g, e, static_cast<u32>(s.size()), kLeafMember);
            if (mine)
                add_to(members, msym, g, s.data());
            else
                before.push(g, s.data());
        }
        if (bank.error()[0]) {
            err->append(bank.error());
            return false;
        }
        out->candidates = members.size();
        if (members.empty()) {
            err->append("no saved genotype of lineage ");
            append_lineage_id(*err, o.lineage);
            err->append(" in the genebank");
            return false;
        }
        // The founder (or the earliest saved member) and its nearest earlier genotypes.
        usize f = 0;
        for (usize i = 0; i < members.size(); ++i)
            if (members[i].parent_hash == 0) {
                f = i;
                break;
            }
        const usize np = before.g.size() - before.head;
        Array<double> nd;
        nd.resize(np);
        AlignScratch sc;
        for (usize i = 0; i < np; ++i) {
            const TreeGenome& q = before.g[before.head + i];
            nd[i] = align_cyclic(msym.data() + members[f].off, members[f].len, before.sym.data() + q.off, q.len, &sc)
                        .distance;
        }
        const usize want_nb = o.neighbours < np ? o.neighbours : np;
        const usize nb = want_nb < cap / 2 ? want_nb : cap / 2;
        Array<u8> taken;
        taken.resize(np);
        Array<usize> picks;
        for (usize k = 0; k < nb; ++k) {
            usize bi = np;
            for (usize i = 0; i < np; ++i)
                if (!taken[i] && (bi == np || nd[i] < nd[bi]))
                    bi = i;
            if (bi == np)
                break;
            taken[bi] = 1;
            picks.push(bi);
        }
        if (members.size() + picks.size() > cap) {
            out->capped = true;
            thin_families(members, cap - picks.size());
        }
        for (usize i = 0; i < members.size(); ++i)
            add_to(in.leaves, in.sym, members[i], msym.data() + members[i].off);
        for (usize k = 0; k < picks.size(); ++k) {
            TreeGenome q = before.g[before.head + picks[k]];
            q.kind = kLeafNeighbour;
            add_to(in.leaves, in.sym, q, before.sym.data() + q.off);
        }
        for (usize i = 0; i < np; ++i) {
            if (taken[i])
                continue;
            TreeGenome q = before.g[before.head + i];
            q.kind = kLeafPool;
            add_to(in.pool, in.sym, q, before.sym.data() + q.off);
        }
    } else {
        // Pass 1: a reservoir of sample / 2 genotypes from the sample's own stream.
        const usize k = o.sample / 2 ? o.sample / 2 : 1;
        Array<u64> pick;
        Rng rng = Rng::from_base(o.seed ^ 0x7472656573616D70ull); // "treesamp"
        u64 fresh_n = 0;
        while (bank.next(&e)) {
            bool fresh = false;
            seen.get_or_insert(e.hash, &fresh);
            if (!fresh)
                continue;
            ++fresh_n;
            if (pick.size() < k) {
                pick.push(e.hash);
            } else {
                const u64 r = rng.below(fresh_n);
                if (r < k)
                    pick[static_cast<usize>(r)] = e.hash;
            }
        }
        if (bank.error()[0]) {
            err->append(bank.error());
            return false;
        }
        out->candidates = fresh_n;
        // Pass 2: the picks' genomes and their parents'.
        HashMap<u8> want;
        for (usize i = 0; i < pick.size(); ++i)
            want.get_or_insert(pick[i]) = 1;
        GenebankReader b2;
        if (!open_bank(dir, &b2, &abc, err))
            return false;
        HashMap<u8> got;
        Array<TreeGenome> parents;
        Array<u8> psym;
        while (b2.next(&e)) {
            u8* w = want.find(e.hash);
            if (!w)
                continue;
            bool first = false;
            got.get_or_insert(e.hash, &first);
            if (!first || !abc.encode(e.genome, &s) || s.empty())
                continue;
            fill_genome(&g, e, static_cast<u32>(s.size()), kLeafMember);
            if (*w == 1) {
                add_to(in.leaves, in.sym, g, s.data());
                if (e.parent_hash && !want.contains(e.parent_hash))
                    want.get_or_insert(e.parent_hash) = 2; // the parent, if it is saved later on
            } else {
                add_to(parents, psym, g, s.data());
            }
        }
        // Parents saved before their child were passed over: one more pass.
        GenebankReader b3;
        if (!open_bank(dir, &b3, &abc, err))
            return false;
        while (b3.next(&e)) {
            const u8* w = want.find(e.hash);
            if (!w || *w != 2)
                continue;
            bool first = false;
            got.get_or_insert(e.hash, &first);
            if (!first || !abc.encode(e.genome, &s) || s.empty())
                continue;
            fill_genome(&g, e, static_cast<u32>(s.size()), kLeafMember);
            add_to(parents, psym, g, s.data());
        }
        if (in.leaves.size() + parents.size() > cap) {
            out->capped = true;
            thin(parents, cap > in.leaves.size() ? cap - in.leaves.size() : 0);
            if (in.leaves.size() > cap)
                in.leaves.resize(cap);
            if (in.leaves.size() + parents.size() > cap)
                parents.resize(cap - in.leaves.size());
        }
        for (usize i = 0; i < parents.size(); ++i)
            add_to(in.leaves, in.sym, parents[i], psym.data() + parents[i].off);
    }
    return tree_build(in, o, out, err);
}

bool top_lineages(const char* dir, u32 k, TextBuf& out, TextBuf* err) {
    struct Count {
        u64 saves, first, last;
    };
    GenebankReader bank;
    OpAlphabet abc;
    if (!open_bank(dir, &bank, &abc, err))
        return false;
    GatedSet gated;
    if (!load_gated(dir, &gated, err))
        return false;
    HashMap<Count> m;
    BankEntry e;
    while (bank.next(&e)) {
        bool fresh = false;
        Count& c = m.get_or_insert(e.lineage, &fresh);
        if (fresh)
            c.first = e.tick;
        ++c.saves;
        c.last = e.tick;
    }
    char line[160];
    snprintf(line, sizeof line, "%llu lineages with saved genotypes; the %u with the most:\n",
             static_cast<unsigned long long>(m.size()), k);
    out.append(line);
    out.append("  rank  lineage        saves   first save    last save  gated\n");
    Array<u8> taken;
    taken.resize(m.slot_count());
    for (u32 r = 0; r < k; ++r) {
        usize bi = m.slot_count();
        for (usize i = 0; i < m.slot_count(); ++i)
            if (m.slot_used(i) && !taken[i] &&
                (bi == m.slot_count() || m.slot_value(i).saves > m.slot_value(bi).saves))
                bi = i;
        if (bi == m.slot_count())
            break;
        taken[bi] = 1;
        const Count& c = m.slot_value(bi);
        TextBuf id;
        append_lineage_id(id, m.slot_key(bi));
        snprintf(line, sizeof line, "  %4u  %-12s %7llu %12llu %12llu  %s\n", r + 1, id.c_str(),
                 static_cast<unsigned long long>(c.saves), static_cast<unsigned long long>(c.first),
                 static_cast<unsigned long long>(c.last), gated.contains(m.slot_key(bi)) ? "yes" : "no");
        out.append(line);
    }
    return true;
}

} // namespace nova
