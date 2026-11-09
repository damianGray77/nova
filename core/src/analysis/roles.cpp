// Behaviour roles (docs/analysis-plan.md A3; docs/results/profiles.md):
// profiles.csv read back by column name, its samples clustered with k-means
// (k chosen by the mean silhouette in a capped range), each cluster named by
// its leading actions, and per window the role shares and the honest
// complexity measures (working length, distinct ops, action kinds). Reads
// files only; every random choice comes from the analysis's own seed.
#include <cmath>   // log, sqrt
#include <cstdio>  // snprintf
#include <cstring> // memcpy, strcmp

#include "nova/analysis.h"
#include "nova/rng.h"

namespace nova {

namespace {

const char* const kFeatureNames[kRoleFeatures] = {
    "engulf_tries", "engulf_landed", "bites_waste", "inject_tries",  "inject_landed", "inside",    "divides",
    "dispersals",   "dormant",       "disk_compares", "disk_stores", "disk_loads",    "self_loads", "emits",
    "listens",      "gpu_ops",       "npu_ops",     "spawns",        "instructions",  "copy_share"};

// The word a leading feature gives a cluster.
const char* const kFeatureRole[kRoleFeatures] = {
    "predator",  "predator",  "scavenger", "parasite", "parasite", "parasite", "copier",   "wanderer", "wanderer",
    "archivist", "archivist", "repairer",  "repairer", "signaller", "listener", "gpu user", "npu user",
    "spawner",   "busy",      "copier"};

// The action behind each action feature (kRfSpawn: launches and threads).
const u32 kFeatureAction[kRfSpawn] = {kPaEngulfTry,    kPaEngulf,    kPaBiteWaste, kPaInjectTry, kPaInject,
                                      kPaInside,       kPaDivide,    kPaDisperse,  kPaDormant,   kPaDiskCompare,
                                      kPaDiskStore,    kPaDiskLoad,  kPaSelfLoad,  kPaEmit,      kPaListen,
                                      kPaGpu,          kPaNpu};

bool get_u64(Str f, u64* v) { return parse_u64(f, v); }
bool get_u32(Str f, u32* v) {
    u64 t = 0;
    if (!parse_u64(f, &t) || t > 0xFFFFFFFFull)
        return false;
    *v = static_cast<u32>(t);
    return true;
}

// A feature's raw value for a row: actions per full profile (scaled up from a
// sample cut short), the instructions likewise, the copy share as is.
double raw_feature(const ProfileRow& r, u32 f, double full) {
    const double scale = r.ticks ? full / static_cast<double>(r.ticks) : 1.0;
    if (f < kRfSpawn)
        return r.act[kFeatureAction[f]] * scale;
    if (f == kRfSpawn)
        return (static_cast<double>(r.act[kPaLaunch]) + r.act[kPaThread]) * scale;
    if (f == kRfInstr)
        return static_cast<double>(r.instr + r.lane_instr) * scale;
    const u64 all = r.instr;
    return all ? static_cast<double>(r.copies) / static_cast<double>(all) : 0.0;
}

double scaled_feature(const ProfileRow& r, u32 f, double full) {
    const double v = raw_feature(r, f, full);
    return f == kRfCopyShare ? v : log(1.0 + v);
}

double dist(const double* a, const double* b, usize d) {
    double s = 0;
    for (usize j = 0; j < d; ++j) {
        const double t = a[j] - b[j];
        s += t * t;
    }
    return sqrt(s);
}

// The mean silhouette of the points idx (rows of x) under labels.
double silhouette(const Matrix& x, const Array<u32>& idx, const Array<u32>& labels, u32 k) {
    const usize m = idx.size();
    if (m < 2 || k < 2)
        return 0;
    Array<double> sum;
    Array<u32> cnt;
    cnt.resize(k);
    for (usize i = 0; i < m; ++i)
        ++cnt[labels[idx[i]]];
    double total = 0;
    for (usize i = 0; i < m; ++i) {
        const u32 a = labels[idx[i]];
        if (cnt[a] <= 1)
            continue; // a singleton scores 0
        sum.resize(k);
        sum.fill(0);
        for (usize j = 0; j < m; ++j)
            if (j != i)
                sum[labels[idx[j]]] += dist(x.row(idx[i]), x.row(idx[j]), x.cols);
        const double ai = sum[a] / static_cast<double>(cnt[a] - 1);
        double bi = -1;
        for (u32 c = 0; c < k; ++c) {
            if (c == a || cnt[c] == 0)
                continue;
            const double v = sum[c] / cnt[c];
            if (bi < 0 || v < bi)
                bi = v;
        }
        if (bi < 0)
            continue;
        const double mx = ai > bi ? ai : bi;
        total += mx > 0 ? (bi - ai) / mx : 0.0;
    }
    return total / static_cast<double>(m);
}

void csv_name(TextBuf& out, const char* name) {
    for (const char* p = name; *p; ++p)
        out.append_char(*p == ' ' ? '_' : *p);
}

// Quantile q (0..1) of v by nearest rank (v holds whole counts, so sorting
// them as fixed-point keys is exact).
double quantile(const Array<double>& v, double q) {
    if (v.empty())
        return 0;
    Array<u64> keys;
    keys.resize(v.size());
    for (usize i = 0; i < v.size(); ++i)
        keys[i] = static_cast<u64>(v[i] * 1024.0 + 0.5); // (values are counts: exact enough)
    sort_u64(keys.data(), keys.size());
    usize at = static_cast<usize>(q * static_cast<double>(keys.size() - 1) + 0.5);
    if (at >= keys.size())
        at = keys.size() - 1;
    return static_cast<double>(keys[at]) / 1024.0;
}

} // namespace

const char* role_feature_name(u32 f) { return f < kRoleFeatures ? kFeatureNames[f] : ""; }

// --- reading profiles.csv ---------------------------------------------------------------

bool ProfileReader::open(const char* path, TextBuf* err) {
    if (!r_.open(path)) {
        err->append("cannot open ");
        err->append(path);
        return false;
    }
    Str line;
    if (!r_.next(&line) || !h_.parse(line)) {
        err->append(path);
        err->append(": no header");
        return false;
    }
    fields_ = h_.count();
    struct Need {
        const char* name;
        i32* col;
    };
    const Need need[] = {{"tick", &col_tick_},         {"end_tick", &col_end_},    {"ticks", &col_ticks_},
                         {"node", &col_node_},         {"lineage", &col_lineage_}, {"genotype", &col_genotype_},
                         {"genome_len", &col_glen_},   {"role", &col_role_},       {"instr", &col_instr_},
                         {"lane_instr", &col_lane_},   {"work_len", &col_work_},   {"distinct_ops", &col_ops_},
                         {"action_kinds", &col_kinds_}, {"node_bodies", &col_nb_[0]}, {"lin_alive", &col_nb_[1]},
                         {"geno_alive", &col_nb_[2]},  {"nb_left", &col_nb_[3]},   {"nb_right", &col_nb_[4]}};
    for (const Need& n : need) {
        *n.col = h_.find(n.name);
        if (*n.col < 0) {
            err->append(path);
            err->append(": no column ");
            err->append(n.name);
            return false;
        }
    }
    for (u32 a = 0; a < kPaCount; ++a) {
        col_act_[a] = h_.find(profile_action_name(a));
        if (col_act_[a] < 0) {
            err->append(path);
            err->append(": no column ");
            err->append(profile_action_name(a));
            return false;
        }
    }
    const char* const lta[6] = {"lta_zero_fell", "lta_zero_other", "lta_pos_fell",
                                "lta_pos_other", "lta_neg_fell",   "lta_neg_other"};
    for (u32 i = 0; i < 6; ++i) {
        col_lta_[i] = h_.find(lta[i]);
        if (col_lta_[i] < 0) {
            err->append(path);
            err->append(": no column ");
            err->append(lta[i]);
            return false;
        }
    }
    col_copy_[0] = h_.find("op_COPY");
    col_copy_[1] = h_.find("op_WRITE");
    col_copy_[2] = h_.find("op_movii");
    // A row splits into at most 256 fields (next): every column read must be
    // among them, or a hand-made file with a wider header would read past them.
    max_col_ = 0;
    for (const Need& n : need)
        max_col_ = *n.col > max_col_ ? *n.col : max_col_;
    for (u32 a = 0; a < kPaCount; ++a)
        max_col_ = col_act_[a] > max_col_ ? col_act_[a] : max_col_;
    for (u32 i = 0; i < 6; ++i)
        max_col_ = col_lta_[i] > max_col_ ? col_lta_[i] : max_col_;
    for (u32 i = 0; i < 3; ++i)
        max_col_ = col_copy_[i] > max_col_ ? col_copy_[i] : max_col_;
    if (max_col_ >= 256) {
        err->append(path);
        err->append(": a column it reads comes after the 256th");
        return false;
    }
    return true;
}

bool ProfileReader::next(ProfileRow* r) {
    Str line;
    if (!r_.next(&line))
        return false;
    Str f[256];
    const u32 n = csv_split(line, f, 256);
    if ((n < fields_ && n < 256) || n <= static_cast<u32>(max_col_)) {
        snprintf(err_, sizeof err_, "row %llu: %u fields, the header has %u", static_cast<unsigned long long>(rows_ + 2),
                 n, fields_);
        return false;
    }
    *r = ProfileRow{};
    bool ok = get_u64(f[col_tick_], &r->tick) && get_u64(f[col_end_], &r->end_tick) &&
              get_u64(f[col_ticks_], &r->ticks) && get_u32(f[col_node_], &r->node) &&
              parse_lineage_id(f[col_lineage_], &r->lineage) && parse_hex(f[col_genotype_], &r->genotype) &&
              get_u32(f[col_glen_], &r->genome_len) && get_u64(f[col_instr_], &r->instr) &&
              get_u64(f[col_lane_], &r->lane_instr) && get_u32(f[col_work_], &r->work_len) &&
              get_u32(f[col_ops_], &r->distinct_ops) && get_u32(f[col_kinds_], &r->action_kinds);
    for (u32 a = 0; ok && a < kPaCount; ++a)
        ok = get_u32(f[col_act_[a]], &r->act[a]);
    for (u32 i = 0; ok && i < 6; ++i)
        ok = get_u32(f[col_lta_[i]], &r->lta[i]);
    u32* nb[5] = {&r->node_bodies, &r->lin_alive, &r->geno_alive, &r->nb_left, &r->nb_right};
    for (u32 i = 0; ok && i < 5; ++i)
        ok = get_u32(f[col_nb_[i]], nb[i]);
    for (u32 i = 0; ok && i < 3; ++i) {
        u64 v = 0;
        if (col_copy_[i] >= 0 && (ok = get_u64(f[col_copy_[i]], &v)))
            r->copies += v;
    }
    r->role = kRoleIdle;
    for (u32 k = 0; k < kRoleCount; ++k)
        if (f[col_role_].eq(role_name(k)))
            r->role = k;
    if (!ok) {
        snprintf(err_, sizeof err_, "row %llu: a malformed value", static_cast<unsigned long long>(rows_ + 2));
        return false;
    }
    ++rows_;
    return true;
}

bool load_profiles(const char* dir, Array<ProfileRow>* out, TextBuf* err) {
    out->clear();
    TextBuf path;
    path_join(path, dir, "profiles.csv");
    ProfileReader rd;
    if (!rd.open(path.c_str(), err))
        return false;
    ProfileRow r;
    while (rd.next(&r))
        out->push(r);
    if (rd.error()[0]) {
        err->append("profiles.csv: ");
        err->append(rd.error());
        return false;
    }
    return true;
}

bool load_arrivals(const char* dir, Array<ArrivalRecord>* out, TextBuf* err) {
    out->clear();
    TextBuf path;
    path_join(path, dir, "arrivals.csv");
    LineReader lr;
    if (!lr.open(path.c_str())) {
        err->append("no arrivals.csv in the run");
        return false;
    }
    Str line;
    CsvHeader h;
    if (!lr.next(&line) || !h.parse(line)) {
        err->append("arrivals.csv has no header");
        return false;
    }
    const i32 c[7] = {h.find("tick"), h.find("node"), h.find("src"), h.find("lineage"),
                      h.find("genotype"), h.find("len"), h.find("dormant")};
    for (i32 v : c)
        if (v < 0) {
            err->append("arrivals.csv lacks a column (tick, node, src, lineage, genotype, len, dormant)");
            return false;
        }
    Str f[32];
    u64 row = 1;
    while (lr.next(&line)) {
        ++row;
        const u32 n = csv_split(line, f, 32);
        ArrivalRecord a = {};
        bool ok = n >= h.count() && get_u64(f[c[0]], &a.tick) && get_u32(f[c[1]], &a.node) &&
                  get_u32(f[c[2]], &a.src) && parse_lineage_id(f[c[3]], &a.lineage) &&
                  parse_hex(f[c[4]], &a.genotype) && get_u32(f[c[5]], &a.len) && get_u32(f[c[6]], &a.dormant);
        ok = ok && a.node < 1000000; // world.nodes' most (the groups' counting sort sizes by it)
        if (!ok) {
            err->append("arrivals.csv: row ");
            err->append_u64(row);
            err->append(" is malformed");
            return false;
        }
        out->push(a);
    }
    return true;
}

// --- the clusters -------------------------------------------------------------------------------

bool roles_build(const Array<ProfileRow>& rows, const RolesOptions& o, RolesReport* out, TextBuf* err) {
    out->samples = rows.size();
    out->clustered = 0;
    out->k = 0;
    for (double& s : out->silhouette)
        s = 0;
    out->clusters.clear();
    out->labels.clear();
    for (u32 r = 0; r < kRoleCount; ++r)
        out->live[r] = 0;
    out->census_csv.clear();
    out->complexity_csv.clear();
    out->clusters_csv.clear();
    if (rows.empty()) {
        err->append("no samples to cluster");
        return false;
    }
    if (o.kmin < 1 || o.kmax < o.kmin || o.kmax > 15) {
        err->append("k must lie in 1..15, kmin <= kmax");
        return false;
    }
    // The full profile length: the longest sample (profile.ticks).
    double full = 1;
    for (usize i = 0; i < rows.size(); ++i)
        if (static_cast<double>(rows[i].ticks) > full)
            full = static_cast<double>(rows[i].ticks);
    for (usize i = 0; i < rows.size(); ++i)
        ++out->live[rows[i].role];

    // The rows clustered: all, or a uniform reservoir of max_samples.
    Array<u32> pick;
    {
        Rng rr = Rng::from_base(o.seed ^ 0x726F6C6573000000ull); // "roles"
        const usize cap = o.max_samples ? o.max_samples : rows.size();
        for (usize i = 0; i < rows.size(); ++i) {
            if (pick.size() < cap) {
                pick.push(static_cast<u32>(i));
            } else {
                const u64 j = rr.below(i + 1);
                if (j < cap)
                    pick[static_cast<usize>(j)] = static_cast<u32>(i);
            }
        }
        if (pick.size() == cap && cap < rows.size()) { // back in row order
            Array<u64> keys;
            keys.resize(pick.size());
            for (usize i = 0; i < pick.size(); ++i)
                keys[i] = pick[i];
            sort_u64(keys.data(), keys.size());
            for (usize i = 0; i < pick.size(); ++i)
                pick[i] = static_cast<u32>(keys[i]);
        }
    }
    const usize n = pick.size();
    out->clustered = n;
    // Features, then each scaled to zero mean and unit spread (a constant one is 0).
    Matrix x;
    x.resize(n, kRoleFeatures);
    for (usize i = 0; i < n; ++i)
        for (u32 f = 0; f < kRoleFeatures; ++f)
            x.at(i, f) = scaled_feature(rows[pick[i]], f, full);
    double mu[kRoleFeatures] = {}, sd[kRoleFeatures] = {};
    for (u32 f = 0; f < kRoleFeatures; ++f) {
        double s = 0, s2 = 0;
        for (usize i = 0; i < n; ++i)
            s += x.at(i, f);
        mu[f] = s / static_cast<double>(n);
        for (usize i = 0; i < n; ++i)
            s2 += (x.at(i, f) - mu[f]) * (x.at(i, f) - mu[f]);
        sd[f] = sqrt(s2 / static_cast<double>(n));
        for (usize i = 0; i < n; ++i)
            x.at(i, f) = sd[f] > 1e-12 ? (x.at(i, f) - mu[f]) / sd[f] : 0.0;
    }
    // The silhouette's subsample: evenly spaced through the rows (deterministic).
    Array<u32> sub;
    const usize m = n < o.silhouette_sample ? n : o.silhouette_sample;
    for (usize i = 0; i < m; ++i)
        sub.push(static_cast<u32>(i * n / m));
    // k: the best mean silhouette in [kmin, kmax] (the smaller on a tie).
    Matrix best_c, cand_c;
    Array<u32> best_l, cand_l, try_l;
    double best_s = -2;
    u32 best_k = 1;
    const u32 kmax = o.kmax < n ? o.kmax : static_cast<u32>(n);
    for (u32 k = o.kmin; k <= kmax; ++k) {
        double best_in = -1;
        Matrix try_c;
        for (u32 r = 0; r < (o.restarts ? o.restarts : 1); ++r) {
            const double in = kmeans(x, k, o.seed * 1000003ull + k * 101ull + r, 300, &try_c, &try_l);
            if (best_in < 0 || in < best_in) {
                best_in = in;
                cand_c.resize(try_c.rows, try_c.cols);
                cand_c.v.assign(try_c.v.data(), try_c.v.size());
                cand_l.assign(try_l.data(), try_l.size());
            }
        }
        const double s = k >= 2 ? silhouette(x, sub, cand_l, k) : 0.0;
        if (k < 16)
            out->silhouette[k] = s;
        if (s > best_s + 1e-12) {
            best_s = s;
            best_k = k;
            best_c.resize(cand_c.rows, cand_c.cols);
            best_c.v.assign(cand_c.v.data(), cand_c.v.size());
            best_l.assign(cand_l.data(), cand_l.size());
        }
    }
    if (best_l.empty()) { // (kmax below kmin: fewer rows than clusters)
        best_k = 1;
        best_c.resize(1, kRoleFeatures);
        best_l.resize(n);
        best_l.fill(0);
    }
    // Every row's label: its own for clustered rows, else the nearest centroid.
    Array<u32> lab;
    lab.resize(rows.size());
    {
        usize j = 0;
        for (usize i = 0; i < rows.size(); ++i) {
            if (j < n && pick[j] == i) {
                lab[i] = best_l[j++];
                continue;
            }
            double v[kRoleFeatures];
            for (u32 f = 0; f < kRoleFeatures; ++f) {
                const double t = scaled_feature(rows[i], f, full);
                v[f] = sd[f] > 1e-12 ? (t - mu[f]) / sd[f] : 0.0;
            }
            u32 bc = 0;
            double bd = -1;
            for (u32 c = 0; c < best_k; ++c) {
                const double d = dist(v, best_c.row(c), kRoleFeatures);
                if (bd < 0 || d < bd) {
                    bd = d;
                    bc = c;
                }
            }
            lab[i] = bc;
        }
    }
    // Clusters: sizes, raw means, scaled centroids, live roles; by size.
    Array<RoleCluster> cl;
    cl.resize(best_k);
    for (usize i = 0; i < rows.size(); ++i) {
        RoleCluster& c = cl[lab[i]];
        ++c.size;
        ++c.live[rows[i].role];
        for (u32 f = 0; f < kRoleFeatures; ++f)
            c.mean[f] += raw_feature(rows[i], f, full);
    }
    for (u32 c = 0; c < best_k; ++c) {
        for (u32 f = 0; f < kRoleFeatures; ++f) {
            cl[c].mean[f] = cl[c].size ? cl[c].mean[f] / cl[c].size : 0.0;
            cl[c].z[f] = best_c.at(c, f);
        }
    }
    Array<u64> order; // size descending, then index
    for (u32 c = 0; c < best_k; ++c)
        if (cl[c].size)
            order.push((static_cast<u64>(0xFFFFFFFFu - cl[c].size) << 32) | c);
    sort_u64(order.data(), order.size());
    Array<u32> remap;
    remap.resize(best_k);
    for (usize i = 0; i < order.size(); ++i) {
        const u32 c = static_cast<u32>(order[i] & 0xFFFFFFFFu);
        remap[c] = static_cast<u32>(i);
        out->clusters.push(cl[c]);
    }
    out->k = static_cast<u32>(out->clusters.size());
    out->labels.resize(rows.size());
    for (usize i = 0; i < rows.size(); ++i)
        out->labels[i] = remap[lab[i]];
    // Names: the leading features (scaled centroid above 0.3, actions first,
    // the largest first); no action leading and little activity is idle.
    for (usize c = 0; c < out->clusters.size(); ++c) {
        RoleCluster& rc = out->clusters[c];
        for (u32 pass = 0; pass < 2; ++pass) { // actions; then activity and copying
            for (u32 t = 0; t < 3 && rc.nlead < 3; ++t) {
                u32 best = kRoleFeatures;
                for (u32 f = 0; f < kRoleFeatures; ++f) {
                    const bool action = f <= kRfSpawn;
                    if ((pass == 0) != action || rc.z[f] <= 0.3)
                        continue;
                    bool used = false;
                    for (u32 j = 0; j < rc.nlead; ++j)
                        used = used || rc.lead[j] == f;
                    if (!used && rc.mean[f] > 0 && (best == kRoleFeatures || rc.z[f] > rc.z[best]))
                        best = f;
                }
                if (best == kRoleFeatures)
                    break;
                rc.lead[rc.nlead++] = best;
            }
        }
        const char* word = rc.nlead ? kFeatureRole[rc.lead[0]] : "idle";
        u32 same = 0;
        for (usize j = 0; j < c; ++j) {
            const RoleCluster& o2 = out->clusters[j];
            const char* w2 = o2.nlead ? kFeatureRole[o2.lead[0]] : "idle";
            same += strcmp(word, w2) == 0 ? 1u : 0u;
        }
        if (same)
            snprintf(rc.name, sizeof rc.name, "%s %u", word, same + 1);
        else
            snprintf(rc.name, sizeof rc.name, "%s", word);
    }

    // --- the CSVs -----------------------------------------------------------------------
    TextBuf& cc = out->clusters_csv;
    cc.append("cluster,name,samples,share");
    for (u32 f = 0; f < kRoleFeatures; ++f) {
        cc.append(",mean_");
        cc.append(kFeatureNames[f]);
    }
    for (u32 r = 0; r < kRoleCount; ++r) {
        cc.append(",live_");
        cc.append(role_name(r));
    }
    cc.append(",leading\n");
    for (usize c = 0; c < out->clusters.size(); ++c) {
        const RoleCluster& rc = out->clusters[c];
        cc.append_u64(c);
        cc.append_char(',');
        csv_name(cc, rc.name);
        cc.append_char(',');
        cc.append_u64(rc.size);
        cc.append_char(',');
        cc.append_f64(static_cast<double>(rc.size) / static_cast<double>(rows.size()));
        for (u32 f = 0; f < kRoleFeatures; ++f) {
            cc.append_char(',');
            cc.append_f64(rc.mean[f]);
        }
        for (u32 r = 0; r < kRoleCount; ++r) {
            cc.append_char(',');
            cc.append_u64(rc.live[r]);
        }
        cc.append_char(',');
        for (u32 j = 0; j < rc.nlead; ++j) {
            if (j)
                cc.append_char(' ');
            cc.append(kFeatureNames[rc.lead[j]]);
        }
        cc.append_char('\n');
    }
    // Windows by sample start.
    const u64 win = o.window ? o.window : 1;
    u64 first_w = ~0ull, last_w = 0;
    for (usize i = 0; i < rows.size(); ++i) {
        const u64 w = rows[i].tick / win;
        first_w = w < first_w ? w : first_w;
        last_w = w > last_w ? w : last_w;
    }
    TextBuf& rc = out->census_csv;
    rc.append("window_first,window_last,samples");
    for (usize c = 0; c < out->clusters.size(); ++c) {
        rc.append(",");
        csv_name(rc, out->clusters[c].name);
    }
    for (u32 r = 0; r < kRoleCount; ++r) {
        rc.append(",live_");
        rc.append(role_name(r));
    }
    rc.append_char('\n');
    TextBuf& xc = out->complexity_csv;
    xc.append("window_first,window_last,samples,genome_len_mean,work_len_mean,work_len_median,work_len_p90,work_len_max,"
              "working_share_mean,distinct_ops_mean,distinct_ops_median,distinct_ops_p90,distinct_ops_max,"
              "action_kinds_mean,action_kinds_median,action_kinds_p90,action_kinds_max\n");
    Array<u32> in_w;
    Array<double> wl, dops, ak;
    // The rows bucketed by window in one sort -- the window (from the first)
    // in the high 32 bits, the row in the low, so a window's rows keep their
    // file order -- rather than a pass over every row per window, which a
    // long run with a short --window made hours long.
    Array<u64> by_w;
    by_w.resize(rows.size());
    for (usize i = 0; i < rows.size(); ++i)
        by_w[i] = ((rows[i].tick / win - first_w) << 32) | static_cast<u64>(i);
    sort_u64(by_w.data(), by_w.size());
    for (usize at = 0; at < by_w.size();) {
        const u64 w = (by_w[at] >> 32) + first_w;
        in_w.clear();
        while (at < by_w.size() && (by_w[at] >> 32) + first_w == w)
            in_w.push(static_cast<u32>(by_w[at++] & 0xFFFFFFFFull));
        if (in_w.empty())
            continue;
        const double ns = static_cast<double>(in_w.size());
        rc.append_u64(w * win);
        rc.append_char(',');
        rc.append_u64(w * win + win - 1);
        rc.append_char(',');
        rc.append_u64(in_w.size());
        Array<u32> cnt, live;
        cnt.resize(out->clusters.size());
        live.resize(kRoleCount);
        double glen = 0, share = 0;
        wl.clear();
        dops.clear();
        ak.clear();
        for (usize j = 0; j < in_w.size(); ++j) {
            const ProfileRow& r = rows[in_w[j]];
            ++cnt[out->labels[in_w[j]]];
            ++live[r.role];
            glen += r.genome_len;
            const u32 cap = r.genome_len < kProfileGenomeMax ? r.genome_len : kProfileGenomeMax;
            share += cap ? static_cast<double>(r.work_len) / cap : 0.0;
            wl.push(r.work_len);
            dops.push(r.distinct_ops);
            ak.push(r.action_kinds);
        }
        for (usize c = 0; c < cnt.size(); ++c) {
            rc.append_char(',');
            rc.append_f64(cnt[c] / ns);
        }
        for (u32 r = 0; r < kRoleCount; ++r) {
            rc.append_char(',');
            rc.append_f64(live[r] / ns);
        }
        rc.append_char('\n');
        auto stats4 = [&xc](Array<double>& v) {
            double s = 0, mx = 0;
            for (usize i = 0; i < v.size(); ++i) {
                s += v[i];
                mx = v[i] > mx ? v[i] : mx;
            }
            xc.append_char(',');
            xc.append_f64(v.empty() ? 0 : s / static_cast<double>(v.size()));
            xc.append_char(',');
            xc.append_f64(quantile(v, 0.5));
            xc.append_char(',');
            xc.append_f64(quantile(v, 0.9));
            xc.append_char(',');
            xc.append_f64(mx);
        };
        xc.append_u64(w * win);
        xc.append_char(',');
        xc.append_u64(w * win + win - 1);
        xc.append_char(',');
        xc.append_u64(in_w.size());
        xc.append_char(',');
        xc.append_f64(glen / ns);
        // work_len: mean, median, p90, max; then the working share
        {
            double s = 0, mx = 0;
            for (usize i = 0; i < wl.size(); ++i) {
                s += wl[i];
                mx = wl[i] > mx ? wl[i] : mx;
            }
            xc.append_char(',');
            xc.append_f64(s / ns);
            xc.append_char(',');
            xc.append_f64(quantile(wl, 0.5));
            xc.append_char(',');
            xc.append_f64(quantile(wl, 0.9));
            xc.append_char(',');
            xc.append_f64(mx);
            xc.append_char(',');
            xc.append_f64(share / ns);
        }
        stats4(dops);
        stats4(ak);
        xc.append_char('\n');
    }
    return true;
}

bool roles_analyse(const char* dir, const RolesOptions& o, RolesReport* out, TextBuf* err) {
    Array<ProfileRow> rows;
    if (!load_profiles(dir, &rows, err))
        return false;
    return roles_build(rows, o, out, err);
}

void roles_summary(const RolesReport& r, TextBuf& out) {
    char line[512];
    snprintf(line, sizeof line, "roles: %llu samples, %llu clustered; k = %u by the mean silhouette\n",
             static_cast<unsigned long long>(r.samples), static_cast<unsigned long long>(r.clustered), r.k);
    out.append(line);
    out.append("  silhouette by k:");
    for (u32 k = 2; k < 16; ++k) {
        if (r.silhouette[k] == 0)
            continue;
        snprintf(line, sizeof line, " %u:%.3f", k, r.silhouette[k]);
        out.append(line);
    }
    out.append("\n\nclusters (named by their leading actions; means per sample of a full profile):\n");
    for (usize c = 0; c < r.clusters.size(); ++c) {
        const RoleCluster& rc = r.clusters[c];
        snprintf(line, sizeof line, "  %-14s %6.1f%%  ", rc.name,
                 r.samples ? 100.0 * rc.size / static_cast<double>(r.samples) : 0.0);
        out.append(line);
        if (rc.nlead == 0)
            out.append("(no action above the average; ");
        else
            out.append("(");
        for (u32 j = 0; j < rc.nlead; ++j) {
            snprintf(line, sizeof line, "%s%s %.3g", j ? ", " : "", kFeatureNames[rc.lead[j]], rc.mean[rc.lead[j]]);
            out.append(line);
        }
        snprintf(line, sizeof line, "%sinstructions %.0f)\n", rc.nlead ? "; " : "", rc.mean[kRfInstr]);
        out.append(line);
    }
    out.append("\nthe live rules against the clusters (samples):\n  cluster       ");
    for (u32 k = 0; k < kRoleCount; ++k) {
        snprintf(line, sizeof line, " %9s", role_name(k));
        out.append(line);
    }
    out.append_char('\n');
    for (usize c = 0; c < r.clusters.size(); ++c) {
        snprintf(line, sizeof line, "  %-14s", r.clusters[c].name);
        out.append(line);
        for (u32 k = 0; k < kRoleCount; ++k) {
            snprintf(line, sizeof line, " %9u", r.clusters[c].live[k]);
            out.append(line);
        }
        out.append_char('\n');
    }
    out.append("  all           ");
    for (u32 k = 0; k < kRoleCount; ++k) {
        snprintf(line, sizeof line, " %9u", r.live[k]);
        out.append(line);
    }
    out.append("\n\nA name summarises a cluster's actions; it is not a proof of the role.\n");
}

} // namespace nova
