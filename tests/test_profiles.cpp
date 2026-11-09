// Behaviour profiles (docs/analysis-plan.md A3; docs/results/profiles.md):
// sampling never changes a world, hand-written predators, parasites and
// copiers get their roles and clusters, the codons that ran match a trace,
// the group measures find planted effects and nothing in shuffled data, and
// profiles.csv reads back by column name.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/analysis.h"
#include "nova/keys.h"
#include "nova/profile.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// --- worlds for the determinism gates --------------------------------------------

// Profiles on: a round every 25 ticks, 6 bodies a node, each followed 20.
const char* const kProfilesOn = "profile.enabled = true\nprofile.every = 25\nprofile.per_node = 6\nprofile.ticks = 20\n";
const char* const kProfilesOff = "profile.enabled = false\n";

// A busy timed world with Phase 10 on (as test_ecology's): actions in
// progress, threads and E cores, bites, threads ending, frozen deaths.
const char* const kTimedWorld = "seed = 42\nworld.model = soup\nworld.nodes = 16\nworld.chords = 8\n"
                                "world.soup_size = 2048\nworld.millicores = 400\ncpu.instr_per_core = 25000\n"
                                "cpu.wear = 0.002\nisa.table = nova_v2\nspark.timing = last\nspark.initial = 20\n"
                                "spark.rate = 0.25\nspark.grant = 20\nspark.min_len = 8\nspark.max_len = 24\n"
                                "seed.genome = NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE INJECT NOP1 DIVIDE\n"
                                "seed.at = 1000\nseed.millicores = 100\nhazard.copy = 0.005\nhazard.flaw = 0.001\n"
                                "hazard.rot = 0.0000005\nheat.cpu_gain = 36\ncost.table = home_v1\n"
                                "cpu.threads = true\ncpu.e_share = 0.5\nmem.decay = 0.0003\nvent.spacing = 1024\n"
                                "engulf.bite = true\nengulf.codons_per_tick = 8\nlife.threads_end = true\n"
                                "life.frozen_ticks = 200\n";

// The procedural world with dispersal (arrivals), dormancy and drift, the disk
// (repairs), signals and the salvage rule.
const char* proc_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append(kSoupWorldProc);
    t.append("life.frozen_ticks = 40\nlife.dormancy = true\nmem.upkeep = 6\nworld.disk = 1024\n"
             "signal.channels = 4\nsalvage.ratio = 4\n");
    return t.c_str();
}

// Warps (as test_warp's busy world): GPU-born bodies, host instructions,
// launches, lanes.
const char* warp_world() {
    static TextBuf t;
    if (t.size())
        return t.c_str();
    t.append("seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 12\nworld.soup_size = 2048\n"
             "world.millicores = 300\nworld.gpus = 300\ngpu.warps = true\nwarp.device = cpu\n"
             "edge.bandwidth = 8\ncpu.instr_per_core = 2500\ncpu.wear = 0.002\ngpu.wear = 0.002\n"
             "spark.timing = last\nspark.initial = 20\nspark.rate = 0.5\nspark.grant = 20\nspark.gpu_grant = 6\n"
             "spark.min_len = 8\nspark.max_len = 32\nhazard.copy = 0.005\nhazard.flaw = 0.001\n"
             "hazard.rot = 0.00005\nsalvage.ratio = 4\nworld.disk = 512\nheat.gpu_gain = 30\n"
             "seed.at = 600\nseed.form = gpu\nseed.millicores = 0\nseed.gpus = 64\nseed.genome = "
             "SENSE ALLOC SHR SHR SHR SHR SHR NOP1 COPY DJNZ NOP0 ZERO INC INC ROTB "
             "ZERO INC SHL SHL SHL ALLOC ZERO ROTB INC SHL SHL SHL SHL SHL DIVIDE ZERO ZERO\n");
    return t.c_str();
}

bool same_file(const char* a, const char* b) {
    Array<u8> x, y;
    return read_file(a, &x) && read_file(b, &y) && x.size() == y.size() &&
           (x.size() == 0 || memcmp(x.data(), y.data(), x.size()) == 0);
}

u64 lines_in(const char* path) {
    Array<u8> x;
    if (!read_file(path, &x))
        return 0;
    u64 n = 0;
    for (usize i = 0; i < x.size(); ++i)
        n += x[i] == '\n';
    return n;
}

// One world, profiles off and on, at 1, 4 and 20 threads and under shuffled
// dispatch: one hash. With profiles on, the files are the same at 1 and 20
// threads, samples were taken, and the census adds up.
void gate(const char* name, const char* world, u64 ticks, bool arrivals) {
    u64 hashes[5] = {};
    const u32 threads[5] = {1, 1, 4, 20, 4};
    u64 samples = 0, rows = 0, arr = 0, census = 0, gpu_samples = 0, lanes = 0;
    char prof_path[5][96], arr_path[5][96];
    Array<u32> order;
    for (u32 v = 0; v < 5; ++v) {
        TextBuf cfg;
        cfg.append(world);
        cfg.append(v == 0 ? kProfilesOff : kProfilesOn);
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        snprintf(prof_path[v], sizeof prof_path[v], "nova_test_profiles_%s_%u.csv", name, v);
        snprintf(arr_path[v], sizeof arr_path[v], "nova_test_arrivals_%s_%u.csv", name, v);
        TextBuf err;
        if (v > 0) {
            CHECK(run.open_profiles(prof_path[v], &err));
            CHECK(run.open_arrivals(arr_path[v], &err));
        }
        if (v == 4) { // (the pool keeps the order's address: it outlives the run)
            const u32 n = run.world().node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            Rng r(77, 0, 0);
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= ticks; ++t) {
            run.run_until(t);
            if (v == 1) {
                for (u32 n = 0; n < run.world().node_count(); ++n) {
                    const Profiles* P = run.world().node_profiles(n);
                    for (usize i = 0; P && i < P->finished().size(); ++i) {
                        ++samples;
                        gpu_samples += P->finished()[i].form == kFormGpu;
                        lanes += P->finished()[i].lane_instr;
                    }
                }
                arr += run.world().arrivals().size();
                if (run.world().profile_census().samples) {
                    const ProfileCensus& c = run.world().profile_census();
                    u32 sum = 0;
                    for (u32 k = 0; k < kRoleCount; ++k)
                        sum += c.role[k];
                    CHECK(sum == c.samples);
                    census = c.samples;
                }
            }
        }
        hashes[v] = run.world().hash();
        run.finish();
        if (v == 1)
            rows = lines_in(prof_path[v]);
        if (v == 0)
            CHECK(run.world().node_profiles(0) == nullptr && run.world().profile_census().samples == 0);
    }
    for (u32 v = 1; v < 5; ++v)
        CHECK_EQ_U64(hashes[v], hashes[0]);
    CHECK(samples > 0 && rows == samples + 1 && census > 0);
    CHECK(same_file(prof_path[1], prof_path[3]) && same_file(prof_path[1], prof_path[4]));
    CHECK(same_file(arr_path[1], arr_path[3]) && same_file(arr_path[1], arr_path[4]));
    if (arrivals)
        CHECK(arr > 0 && lines_in(arr_path[1]) == arr + 1);
    printf("  %s: %llu ticks, profiles on and off hash %016llx at 1, 4, 20 threads and shuffled; %llu samples "
           "(%llu GPU-born, %llu lane instructions), %llu arrivals logged, last census %llu\n",
           name, static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(hashes[0]),
           static_cast<unsigned long long>(samples), static_cast<unsigned long long>(gpu_samples),
           static_cast<unsigned long long>(lanes), static_cast<unsigned long long>(arr),
           static_cast<unsigned long long>(census));
    for (u32 v = 1; v < 5; ++v) {
        remove(prof_path[v]);
        remove(arr_path[v]);
    }
}

void test_hash_unchanged() {
    gate("timed", kTimedWorld, 150, false);
    gate("proc", proc_world(), 150, true);
    gate("warp", warp_world(), 120, true);
    // Sampling isn't in snapshots: a run resumed with profiles on gives the
    // uninterrupted hash.
    TextBuf cfg;
    cfg.append(proc_world());
    cfg.append(kProfilesOn);
    Run a;
    CHECK(make_run(a, cfg.c_str(), 4));
    a.run_until(60); // mid-round: samples under way
    ByteWriter snap;
    a.snapshot(snap);
    a.run_until(150);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    CHECK(b.world().node_profiles(0) && b.world().node_profiles(0)->active() == 0);
    b.run_until(150);
    CHECK_EQ_U64(b.world().hash(), a.world().hash());
}

// --- hand-written roles ---------------------------------------------------------------

struct RoleSoup {
    SoupParams p;
    Soup s;
    ProfileParams pp;
    Profiles prof;
    u64 pred[3], para[3], copy[3]; // serials
};

// Three predators each among 40 surfaceless prey, three parasites each beside
// four hosts whose surface their key fits, and three copiers (the 8-codon
// sketch), far apart in a 16K soup; every living body followed every round
// (10 ticks) for 60 ticks.
bool role_soup(RoleSoup& f, Array<ProfileSample>* out) {
    if (!make_params(&f.p, kQuiet, 16384))
        return false;
    f.s.init(&f.p, 23, 0);
    f.s.begin_tick(0);
    for (u32 g = 0; g < 3; ++g) {
        const u32 at = 100 + 1000 * g;
        // (12 codons: a third of it, 4, swallows a 4-codon prey whole; docs/bites-plan.md B2)
        const u32 b = plant(f.s, at, "ENGULF INC INC INC INC INC INC INC INC INC INC INC", 40);
        if (b == kNone)
            return false;
        f.pred[g] = f.s.body(b).serial;
        for (u32 k = 0; k < 40; ++k)
            if (plant(f.s, at + 14 + 6 * k, "INC INC INC INC", 5) == kNone)
                return false;
    }
    for (u32 g = 0; g < 3; ++g) {
        const u32 at = 4000 + 1000 * g;
        const u32 b = plant(f.s, at, "ZERO INC INC INC INC INC INC ROT ZERO INC INC INJECT NOP0 NOP0 NOP0 NOP0", 40);
        if (b == kNone)
            return false;
        f.para[g] = f.s.body(b).serial;
        for (u32 k = 0; k < 4; ++k)
            if (plant(f.s, at + 30 + 30 * k, "NOP1 NOP1 NOP1 NOP1 INC INC INC INC INC INC INC INC", 5) == kNone)
                return false;
    }
    for (u32 g = 0; g < 3; ++g) {
        const u32 b = plant(f.s, 9000 + 2000 * g, "SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE", 100);
        if (b == kNone)
            return false;
        f.copy[g] = f.s.body(b).serial;
    }
    f.pp.enabled = true;
    f.pp.every = 10;
    f.pp.per_node = 4096;
    f.pp.ticks = 10;
    f.pp.arrivals = false;
    f.prof.init(&f.pp, 23, 0);
    f.s.set_profiles(&f.prof);
    for (u64 t = 0; t < 60; ++t) {
        f.s.tick(t);
        f.prof.on_tick_end(f.s, t);
        for (usize i = 0; i < f.prof.finished().size(); ++i)
            out->push(f.prof.finished()[i]);
        f.prof.clear_finished();
    }
    return soup_ok(f.s);
}

// The cluster most of a planted kind's samples (of their own live role) land in.
u32 majority(const RolesReport& r, const Array<ProfileSample>& s, const u64* serials, u32 role, u32* total) {
    u32 count[16] = {};
    *total = 0;
    for (usize i = 0; i < s.size(); ++i) {
        bool mine = false;
        for (u32 g = 0; g < 3; ++g)
            mine = mine || s[i].serial == serials[g];
        if (!mine || role_by_rules(s[i].act) != role)
            continue;
        ++count[r.labels[i] < 16 ? r.labels[i] : 15];
        ++*total;
    }
    u32 best = 0;
    for (u32 c = 1; c < 16; ++c)
        best = count[c] > count[best] ? c : best;
    return best;
}

void test_roles() {
    RoleSoup* f = new RoleSoup;
    Array<ProfileSample> samples;
    CHECK(role_soup(*f, &samples));
    // The first round's samples of the planted bodies: their live roles.
    u32 seen = 0;
    for (usize i = 0; i < samples.size(); ++i) {
        const ProfileSample& x = samples[i];
        if (x.start != 0)
            continue;
        for (u32 g = 0; g < 3; ++g) {
            if (x.serial == f->pred[g]) {
                CHECK(role_by_rules(x.act) == kRolePredator && x.act[kPaEngulf] > 0 && x.why == kProfileDone);
                ++seen;
            }
            if (x.serial == f->para[g]) {
                CHECK(role_by_rules(x.act) == kRoleParasite && x.act[kPaInject] > 0);
                ++seen;
            }
            if (x.serial == f->copy[g]) {
                CHECK(role_by_rules(x.act) == kRoleCopier && x.act[kPaDivide] > 0);
                CHECK(x.ops[static_cast<u32>(Op::copy)] > 0 && profile_working_length(x) == 7); // all but DJNZ's template
                ++seen;
            }
        }
    }
    CHECK(seen == 9);

    // profiles.csv, written as a run writes it and read back by column name.
    const char* dir = "nova_test_profiles_roles";
    CHECK(make_dirs(dir));
    TextBuf csv;
    profile_csv_header(f->p.table, csv);
    for (usize i = 0; i < samples.size(); ++i)
        profile_csv_row(f->p.table, samples[i], csv);
    TextBuf path;
    path_join(path, dir, "profiles.csv");
    CHECK(write_file(path.c_str(), csv.view().p, csv.size()));
    Array<ProfileRow> rows;
    TextBuf err;
    CHECK(load_profiles(dir, &rows, &err));
    CHECK(rows.size() == samples.size());
    bool same = rows.size() == samples.size();
    for (usize i = 0; same && i < rows.size(); ++i) {
        const ProfileRow& r = rows[i];
        const ProfileSample& x = samples[i];
        same = r.tick == x.start && r.end_tick == x.end && r.ticks == x.end - x.start + 1 && r.node == x.node &&
               r.genotype == x.genotype && r.genome_len == x.glen && r.instr == x.instr &&
               r.work_len == profile_working_length(x) && r.distinct_ops == profile_distinct_ops(x) &&
               r.action_kinds == action_kinds(x.act) && r.role == role_by_rules(x.act) &&
               r.copies == x.ops[static_cast<u32>(Op::copy)] + x.ops[static_cast<u32>(Op::write)] &&
               memcmp(r.act, x.act, sizeof r.act) == 0 && memcmp(r.lta, x.lta, sizeof r.lta) == 0;
    }
    CHECK(same);

    // Clusters: the three kinds land in three different ones, named for them.
    RolesOptions o;
    RolesReport r;
    CHECK(roles_build(rows, o, &r, &err));
    u32 np = 0, ni = 0, nc = 0;
    const u32 cp = majority(r, samples, f->pred, kRolePredator, &np);
    const u32 ci = majority(r, samples, f->para, kRoleParasite, &ni);
    const u32 cc = majority(r, samples, f->copy, kRoleCopier, &nc);
    CHECK(np > 0 && ni > 0 && nc > 0);
    CHECK(cp != ci && cp != cc && ci != cc);
    if (cp < r.clusters.size() && ci < r.clusters.size() && cc < r.clusters.size()) {
        CHECK(strncmp(r.clusters[cp].name, "predator", 8) == 0);
        CHECK(strncmp(r.clusters[ci].name, "parasite", 8) == 0);
        CHECK(strncmp(r.clusters[cc].name, "copier", 6) == 0);
        printf("  roles: %llu samples, k = %u; predators in '%s', parasites in '%s', copiers in '%s'\n",
               static_cast<unsigned long long>(rows.size()), r.k, r.clusters[cp].name, r.clusters[ci].name,
               r.clusters[cc].name);
    }
    // The same rows give the same clusters (the analysis's own seed).
    RolesReport r2;
    CHECK(roles_build(rows, o, &r2, &err) && r2.k == r.k && r2.labels.size() == r.labels.size() &&
          memcmp(r2.labels.data(), r.labels.data(), r.labels.size() * sizeof(u32)) == 0);
    CHECK(r.census_csv.size() > 0 && r.complexity_csv.size() > 0 && r.clusters_csv.size() > 0);
    remove(path.c_str());
    delete f;
}

// --- the codons that ran ----------------------------------------------------------------

// A traced run, one instruction at a time: the bitmap holds exactly the genome
// codons the IP stood on (the jump skips three, its template and the match).
void test_codon_trace() {
    SoupParams* p = new SoupParams;
    Soup* s = new Soup;
    CHECK(make_params(p, kQuiet, 1024));
    s->init(p, 23, 0);
    s->begin_tick(0);
    const u32 at = 100, len = 12;
    const u32 b = plant(*s, at, "JMP NOP1 INC INC INC NOP0 DEC DEC ZERO IFZ JMP NOP1", 100);
    CHECK(b != kNone);
    ProfileParams pp;
    pp.enabled = true;
    pp.every = 1000000;
    pp.per_node = 1;
    pp.ticks = 1000;
    Profiles* prof = new Profiles;
    prof->init(&pp, 1, 0);
    s->set_profiles(prof);
    CHECK(prof->follow(*s, b, 0) && prof->active() == 1);
    bool ran[16] = {};
    const u32 c = s->body(b).cpus;
    for (u32 i = 0; i < 60; ++i) {
        const u32 ip = s->cpu(c).ip;
        if (ip >= at && ip < at + len)
            ran[ip - at] = true;
        s->run_cpu(c, 1);
    }
    prof->finish_all(0);
    CHECK(prof->finished().size() == 1);
    if (prof->finished().size() == 1) {
        const ProfileSample& x = prof->finished()[0];
        u32 expect = 0;
        bool same = true;
        for (u32 i = 0; i < len; ++i) {
            const bool bit = (x.bits[i >> 3] >> (i & 7)) & 1;
            same = same && bit == ran[i];
            expect += ran[i];
        }
        CHECK(same && profile_working_length(x) == expect && expect < len);
        CHECK(x.instr == 60 && x.ops[static_cast<u32>(Op::jmp)] > 0 && x.ops[static_cast<u32>(Op::inc)] == 0);
        CHECK(x.glen == len && x.gstart == at && x.why == kProfileDone);
        printf("  codons: %u of %u ran, as traced (%u distinct ops)\n", expect, len, profile_distinct_ops(x));
    }
    delete prof;
    delete s;
    delete p;
}

// --- the group measures, planted and shuffled ----------------------------------------------

void sort_by_tick(Array<ArrivalRecord>& a) {
    Array<u64> keys;
    for (usize i = 0; i < a.size(); ++i)
        keys.push((a[i].tick << 24) | i);
    sort_u64(keys.data(), keys.size());
    Array<ArrivalRecord> b;
    for (usize i = 0; i < keys.size(); ++i)
        b.push(a[keys[i] & 0xFFFFFF]);
    a.assign(b.data(), b.size());
}

void test_groups() {
    GroupOptions o;
    Rng r(5, 0, 0);
    // Moving together: lineages arriving at a node in bursts of three within
    // a tick, among single arrivals; then the same with the labels shuffled.
    Array<ArrivalRecord> a;
    for (u32 i = 0; i < 600; ++i) {
        ArrivalRecord x = {};
        x.node = static_cast<u32>(r.below(8));
        x.tick = r.below(5000);
        x.lineage = (1ull << 32) | (1 + r.below(6));
        x.len = 20;
        for (u32 k = 0; k < 3; ++k) {
            ArrivalRecord y = x;
            y.tick += k == 2 ? 1 : 0;
            y.src = static_cast<u32>(r.below(4));
            a.push(y);
        }
    }
    for (u32 i = 0; i < 2000; ++i) {
        ArrivalRecord x = {};
        x.node = static_cast<u32>(r.below(8));
        x.tick = r.below(5000);
        x.lineage = (1ull << 32) | (1 + r.below(6));
        x.src = static_cast<u32>(r.below(4));
        a.push(x);
    }
    sort_by_tick(a);
    GroupMeasure all, apart;
    moving_together(a, o, &all, &apart);
    CHECK(all.found && apart.found && all.observed > 2 * all.expected);
    printf("  moving together, planted: %.0f same-lineage pairs against %.1f +- %.1f by chance (z %.1f)\n",
           all.observed, all.expected, all.sd, all.z);
    for (usize i = a.size(); i > 1; --i) { // shuffled: the labels no longer follow the times
        const usize j = static_cast<usize>(r.below(i));
        const u64 t = a[i - 1].lineage;
        a[i - 1].lineage = a[j].lineage;
        a[j].lineage = t;
    }
    moving_together(a, o, &all, &apart);
    CHECK(!all.found && !apart.found && all.n > 0);
    printf("  moving together, shuffled: %.0f against %.1f +- %.1f (z %.2f)\n", all.observed, all.expected, all.sd,
           all.z);

    // Kin clustering: neighbours of the same lineage (genome) far above the
    // census's chance; then at chance.
    Array<ProfileRow> rows;
    for (u32 pass = 0; pass < 2; ++pass) {
        rows.clear();
        for (u32 i = 0; i < 400; ++i) {
            ProfileRow x;
            x.lineage = (3ull << 32) | 1;
            x.node_bodies = 100;
            x.lin_alive = 10;
            x.geno_alive = 5;
            const double pl = pass == 0 ? 0.6 : 9.0 / 99.0, pg = pass == 0 ? 0.3 : 4.0 / 99.0;
            x.nb_left = kNbFound | (r.chance(pl) ? kNbLineage : 0u) | (r.chance(pg) ? kNbGenotype : 0u);
            x.nb_right = kNbFound | (r.chance(pl) ? kNbLineage : 0u) | (r.chance(pg) ? kNbGenotype : 0u);
            rows.push(x);
        }
        GroupMeasure kl, kg;
        kin_clustering(rows, o, &kl, &kg);
        CHECK(kl.n == 400 && kg.n == 400); // one side a sample (the right): an adjacency counts once
        CHECK(pass == 0 ? (kl.found && kg.found) : (!kl.found && !kg.found));
        printf("  kin clustering, %s: lineage z %.1f, genome z %.1f\n", pass == 0 ? "planted" : "chance", kl.z,
               kg.z);
    }

    // Listening then acting: a branch that follows the heard value; then one
    // that ignores it.
    for (u32 pass = 0; pass < 2; ++pass) {
        rows.clear();
        for (u32 i = 0; i < 150; ++i) {
            ProfileRow x;
            for (u32 k = 0; k < 12; ++k) {
                const u32 cls = static_cast<u32>(r.below(3));
                const u32 way = pass == 0 ? ((cls == 0) != r.chance(0.1) ? 0u : 1u) : static_cast<u32>(r.below(2));
                ++x.lta[cls * 2 + way];
            }
            rows.push(x);
        }
        GroupMeasure l;
        listening_acting(rows, o, &l);
        CHECK(l.n == 150);
        CHECK(pass == 0 ? l.found : !l.found);
        printf("  listening then acting, %s: %.2f bits against %.2f +- %.2f (z %.1f), %llu samples individually\n",
               pass == 0 ? "planted" : "ignored", l.observed, l.expected, l.sd, l.z,
               static_cast<unsigned long long>(l.extra));
    }
}

// --- settings ------------------------------------------------------------------------------

void test_settings() {
    const Key* k = key_find("profile.enabled");
    CHECK(k && k->def == 1); // on in the engine's defaults (the test runner's fallback turns it off)
    CHECK(key_find("profile.every") && key_find("profile.every")->def == 1000);
    CHECK(key_find("profile.per_node") && key_find("profile.per_node")->def == 4);
    CHECK(key_find("profile.ticks") && key_find("profile.ticks")->def == 50);
    CHECK(key_find("output.profiles") && key_find("output.arrivals"));
    Config c;
    Config::ParseError pe;
    CHECK(c.parse("profile.enabled = true\nprofile.every = 10\nprofile.ticks = 20\n", &pe));
    ProfileParams p;
    CHECK(!profile_params_from_config(c, &p)); // a round longer than the gap between rounds
    // The setup page's check reads them (World::check_settings).
    Config d;
    CHECK(d.parse("world.model = soup\nworld.nodes = 2\nworld.soup_size = 1024\nprofile.per_node = 0\n", &pe));
    World* w = new World;
    CHECK(!w->check_settings(d));
    delete w;
}

} // namespace

void suite_profiles() {
    test_settings();
    test_codon_trace();
    test_groups();
    test_roles();
    test_hash_unchanged();
}
