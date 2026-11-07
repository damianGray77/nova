// The viewer and the live novelty meter (docs/results/viewer.md): A2's live
// columns and "new kinds of life", A5's map of life, world plan W8 (substrates
// on the map, nodes.csv's new columns, the fast region) and disasters X7's
// map marker. Called from suites view (the core) and viewer (the panels).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/analysis.h"
#include "nova/novelty.h"
#include "nova/png.h"
#include "nova/snapshot.h"
#include "nova/stats.h"
#include "nova/view.h"
#include "theme.h"
#include "viewer.h"

using namespace nova;

namespace {

u64 fixed(double x) { return static_cast<u64>(x * kNoveltyFixed + 0.5); }

// kSoupWorldProc with the live novelty meter on (the legacy fallback has it off).
void novelty_world(TextBuf& t) {
    // kSoupWorldProc with copy errors up, so the replicators' mutants make
    // many genotypes.
    const Str w(kSoupWorldProc);
    const Str from("hazard.copy = 0.005\n");
    usize at = 0;
    while (at + from.n <= w.n && !w.sub(at, from.n).eq(from))
        ++at;
    t.append(w.sub(0, at));
    t.append("hazard.copy = 0.03\n");
    if (at + from.n <= w.n)
        t.append(w.sub(at + from.n, w.n - at - from.n));
    t.append("novelty.enabled = true\ndetect.genebank_threshold = 1\n"); // (every member's genotype saved)
}

void remove_dir(const char* dir, const char* const* files, usize n) {
    for (usize i = 0; i < n; ++i) {
        TextBuf p;
        p.append(dir);
        p.append_char('/');
        p.append(files[i]);
        remove(p.c_str());
    }
    RemoveDirectoryA(dir);
}

struct StatsRowLite {
    u64 first, novelty, share, success;
};

// stats.csv's rows: tick_first and the novelty columns, by name.
bool read_novelty_rows(const char* path, Array<StatsRowLite>* out) {
    Array<u8> bytes;
    if (!read_file(path, &bytes))
        return false;
    const Str t(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    usize at = 0;
    CsvHeader h;
    bool head = true;
    i32 cf = -1, cn = -1, cs = -1, cx = -1;
    while (at < t.n) {
        usize e = at;
        while (e < t.n && t.p[e] != '\n')
            ++e;
        const Str line = t.sub(at, e - at);
        at = e + 1;
        if (line.n == 0)
            continue;
        if (head) {
            head = false;
            if (!h.parse(line))
                return false;
            cf = h.find("tick_first");
            cn = h.find("novelty");
            cs = h.find("novel_share");
            cx = h.find("novel_success");
            if (cf < 0 || cn < 0 || cs < 0 || cx < 0)
                return false;
            continue;
        }
        Str f[160];
        const u32 nf = csv_split(line, f, 160);
        StatsRowLite r = {};
        if (nf <= static_cast<u32>(cx))
            return false;
        parse_u64(f[cf], &r.first);
        parse_u64(f[cn], &r.novelty);
        parse_u64(f[cs], &r.share);
        parse_u64(f[cx], &r.success);
        out->push(r);
    }
    return true;
}

// --- A2: the live columns ---------------------------------------------------------

// The live novelty columns are `nova analyse novelty`'s figures on the same
// run: novelty and novel_share exactly (the same genotypes, in the same
// order, through the same model), novel_success exactly as --as-live counts
// (gated by the window's end), and never more than the default (gated at any
// time).
void test_novelty_columns() {
    const char* dir = "nova_test_novelty_run";
    CreateDirectoryA(dir, nullptr);
    TextBuf cfg;
    novelty_world(cfg);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    CHECK(run.novelty() != nullptr);
    TextBuf err;
    const u32 window = 50;
    CHECK(run.open_stats("nova_test_novelty_run/stats.csv", window, &err));
    CHECK(run.open_replay("nova_test_novelty_run/replay.log", 100, &err));
    CHECK(run.open_genebank("nova_test_novelty_run/genebank.txt", &err));
    CHECK(run.open_lineages("nova_test_novelty_run/lineages.csv", &err));
    run.run_until(600);
    const u64 live_fresh = run.novelty()->fresh(), live_saves = run.novelty()->saves();
    run.finish();

    Array<StatsRowLite> rows;
    CHECK(read_novelty_rows("nova_test_novelty_run/stats.csv", &rows));
    CHECK(rows.size() == 12);
    NoveltyOptions o;
    o.window = window;
    o.top = 0;
    o.as_live = true;
    NoveltyReport as_live, any;
    CHECK(novelty_analyse(dir, o, &as_live, &err));
    o.as_live = false;
    CHECK(novelty_analyse(dir, o, &any, &err));
    CHECK(as_live.total.fresh == live_fresh && as_live.total.saves == live_saves && live_fresh > 20);
    u32 matched = 0, with_fresh = 0, mismatches = 0;
    u64 success_live = 0, success_any = 0;
    for (usize i = 0; i < rows.size(); ++i) {
        const StatsRowLite& r = rows[i];
        const NoveltyWindow* w = nullptr;
        const NoveltyWindow* wa = nullptr;
        for (usize k = 0; k < as_live.windows.size(); ++k)
            if (as_live.windows[k].first == r.first) {
                w = &as_live.windows[k];
                wa = &any.windows[k];
            }
        if (!w) {
            mismatches += r.novelty != 0 || r.share != 0 || r.success != 0;
            continue;
        }
        ++matched;
        with_fresh += w->fresh > 0;
        mismatches += r.novelty != fixed(w->innovation());
        mismatches += r.share != fixed(w->novel_share());
        mismatches += r.success != w->novel_success;
        mismatches += wa->innovation() != w->innovation() || r.success > wa->novel_success;
        success_live += r.success;
        success_any += wa->novel_success;
    }
    CHECK(mismatches == 0);
    CHECK(matched >= 8 && with_fresh >= 6);
    if (with_fresh < 6)
        printf("  (matched %u windows, %u with new genotypes)\n", matched, with_fresh);
    CHECK(success_live > 0 && success_live <= success_any);
    printf("  novelty: %u stats windows match nova analyse novelty --as-live; novel_success %llu live, %llu offline "
           "(gated at any time); %llu new genotypes\n",
           matched, static_cast<unsigned long long>(success_live), static_cast<unsigned long long>(success_any),
           static_cast<unsigned long long>(live_fresh));
    const char* files[] = {"stats.csv", "replay.log", "genebank.txt", "lineages.csv"};
    remove_dir(dir, files, 4);
}

// Watching changes nothing: novelty on and off give the same world, and a
// snapshot resumed starts a fresh model.
void test_novelty_hash() {
    TextBuf on, off;
    on.append(kSoupWorldProc);
    on.append(kPhase4All);
    on.append("novelty.enabled = true\n");
    off.append(kSoupWorldProc);
    off.append(kPhase4All);
    off.append("novelty.enabled = false\n");
    Run a, b;
    CHECK(make_run(a, on.c_str(), 4));
    CHECK(make_run(b, off.c_str(), 20));
    CHECK(a.novelty() != nullptr && b.novelty() == nullptr);
    TextBuf err;
    CHECK(a.open_stats("nova_test_novelty_hash.csv", 25, &err));
    a.keep_genebank(true);
    a.run_until(150);
    b.run_until(150);
    CHECK_EQ_U64(a.world().hash(), b.world().hash());
    CHECK(a.novelty()->fresh() > 0);
    // Through a snapshot: the meter starts afresh, the world goes on the same.
    ByteWriter snap;
    a.snapshot(snap);
    Run c;
    CHECK(c.init_from_snapshot(snap.data(), snap.size(), 4, &err));
    CHECK(c.novelty() != nullptr && c.novelty()->saves() == 0 && c.novelty()->gated_count() == 0);
    a.run_until(250);
    b.run_until(250);
    c.run_until(250);
    CHECK_EQ_U64(a.world().hash(), b.world().hash());
    CHECK_EQ_U64(c.world().hash(), b.world().hash());
    a.finish();
    remove("nova_test_novelty_hash.csv");
    // The setting is read with the rest (registry), and off without detection.
    Run d;
    TextBuf nodet;
    nodet.append(kSoupWorldProc);
    nodet.append("detect.enabled = false\nnovelty.enabled = true\n");
    CHECK(make_run(d, nodet.c_str(), 1));
    CHECK(d.novelty() == nullptr && !d.world().novelty_enabled());
}

// "New kinds of life", live: each window's picks are nova analyse novelty
// --as-live's novelty_top.csv rows (the same genotypes, ranks and surprise).
struct PickRow {
    u64 window, rank, node;
    double surprise;
    char name[16];
};

void parse_top(Str t, Array<PickRow>* out) {
    usize at = 0;
    bool head = true;
    while (at < t.n) {
        usize e = at;
        while (e < t.n && t.p[e] != '\n')
            ++e;
        const Str line = t.sub(at, e - at);
        at = e + 1;
        if (line.n == 0)
            continue;
        if (head) {
            head = false;
            continue;
        }
        Str f[9];
        if (csv_split(line, f, 9) < 9)
            continue;
        PickRow r = {};
        parse_u64(f[0], &r.window);
        parse_u64(f[1], &r.rank);
        parse_f64(f[2], &r.surprise);
        const usize nn = f[3].n < 15 ? f[3].n : 15;
        memcpy(r.name, f[3].p, nn);
        parse_u64(f[4], &r.node);
        out->push(r);
    }
}

void test_live_picks() {
    const char* dir = "nova_test_picks_run";
    CreateDirectoryA(dir, nullptr);
    TextBuf cfg;
    novelty_world(cfg);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    run.keep_genebank(true);
    TextBuf err;
    CHECK(run.open_replay("nova_test_picks_run/replay.log", 100, &err));
    CHECK(run.open_genebank("nova_test_picks_run/genebank.txt", &err));
    CHECK(run.open_lineages("nova_test_picks_run/lineages.csv", &err));
    ViewSession s;
    s.attach(&run, 10);
    s.set_novelty_list(100, 5);
    ViewFrame f;
    Array<NoveltyPick> closed, open;
    LifeMap live_map; // A5 below: the map the viewer builds from the frames' saves
    live_map.set_reference(32);
    Array<char> texts;
    for (u32 t = 0; t < 550; ++t) {
        run.step();
        s.after_step();
        if (t % 7 == 0 || t == 549) {
            ViewRequest q;
            s.build(q, &f);
            CHECK(f.novelty && f.novelty_window == 100);
            closed.append(f.picks.data(), f.picks.size());
            open.clear();
            open.append(f.picks_open.data(), f.picks_open.size());
            for (usize i = 0; i < f.saves.size(); ++i) {
                const GenomeEntry& e = f.saves[i];
                live_map.add(e.tick, e.genotype, e.lineage, e.node, e.len, e.name, Str(f.save_text.data() + e.off, e.n));
            }
        }
    }
    run.finish();
    NoveltyOptions o;
    o.window = 100;
    o.top = 5;
    o.as_live = true;
    NoveltyReport r;
    CHECK(novelty_analyse(dir, o, &r, &err));
    Array<PickRow> rows;
    parse_top(r.top_csv.view(), &rows);
    Array<NoveltyPick> all;
    all.append(closed.data(), closed.size());
    all.append(open.data(), open.size());
    CHECK(rows.size() == all.size() && rows.size() >= 5);
    u32 bad = 0;
    for (usize i = 0; i < rows.size() && i < all.size(); ++i) {
        const PickRow& a = rows[i];
        const NoveltyPick& b = all[i];
        bad += a.window != b.window_first || a.rank != b.rank || a.node != b.node || strcmp(a.name, b.name) != 0 ||
               fabs(a.surprise - b.surprise) > 6e-5;
    }
    CHECK(bad == 0);
    printf("  new kinds of life: %u live picks (%u closed, %u open) equal nova analyse novelty --as-live's top\n",
           static_cast<unsigned>(all.size()), static_cast<unsigned>(closed.size()), static_cast<unsigned>(open.size()));

    // A finished run's list is nova analyse novelty's (its default rule:
    // gated at any time), computed on genebank.txt when there's no
    // novelty_top.csv.
    o.as_live = false;
    o.window = kNoveltyListWindow;
    NoveltyReport rd;
    CHECK(novelty_analyse(dir, o, &rd, &err));
    Array<PickRow> drows;
    parse_top(rd.top_csv.view(), &drows);
    RunArchive ar;
    ar.set_life(1, RunArchive::kLifeRefFirst);
    ar.set_novelty(1);
    CHECK(ar.load(dir, &err));
    CHECK(ar.has_novelty() && ar.pick_count() == drows.size() && drows.size() > 0);
    ViewFrame af;
    ar.build(0, ViewRequest{}, &af);
    CHECK(af.life_sent && af.novelty && af.picks.size() == drows.size());
    bool arch_same = af.picks.size() == drows.size();
    for (usize i = 0; i < af.picks.size() && arch_same; ++i)
        arch_same = af.picks[i].window_first == drows[i].window && af.picks[i].rank == drows[i].rank &&
                    strcmp(af.picks[i].name, drows[i].name) == 0 && af.picks[i].n > 0;
    CHECK(arch_same);
    // A5: the same input gives the same points -- a finished run's map from
    // genebank.txt (a first-N reference) is the live map from its frames.
    // (32 genotypes held as the reference both ways; genebank.txt holds the
    // same saves the frames carried.)
    LifeMap arch;
    arch.set_reference(32);
    GenebankReader bank;
    CHECK(bank.open("nova_test_picks_run/genebank.txt"));
    BankEntry e;
    while (bank.next(&e)) {
        char nm[16];
        const usize nn = e.name.n < 15 ? e.name.n : 15;
        memcpy(nm, e.name.p, nn);
        nm[nn] = '\0';
        u32 len = 0;
        for (usize i = 0; i < nn && nm[i] >= '0' && nm[i] <= '9'; ++i)
            len = len * 10 + static_cast<u32>(nm[i] - '0');
        arch.add(e.tick, e.hash, e.lineage, e.node, len, nm, e.genome);
    }
    live_map.finalize();
    arch.finalize();
    CHECK(live_map.points().size() == arch.points().size() && live_map.points().size() > 32 && live_map.final());
    printf("  map of life: %u genotypes, live from the frames and from genebank.txt\n",
           static_cast<unsigned>(live_map.points().size()));
    bool same = live_map.points().size() == arch.points().size();
    for (usize i = 0; i < live_map.points().size() && same; ++i) {
        const LifePoint& p = live_map.points()[i];
        const LifePoint& q = arch.points()[i];
        same = p.genotype == q.genotype && p.x == q.x && p.y == q.y && p.saves == q.saves && p.first == q.first;
    }
    CHECK(same);
    // ... and the archive's own (fitted on the whole run, kLifeRef at most) holds every genotype.
    CHECK(af.life.points().size() == arch.points().size() && af.life.final());
    const char* files[] = {"replay.log", "genebank.txt", "lineages.csv"};
    remove_dir(dir, files, 3);
}

// --- W8 and X7: nodes.csv's new columns ------------------------------------------

const char* const kSubDisWorld = "substrate.enabled = true\n"
                                 "world.disk = 1024\n"
                                 "disaster.enabled = true\n"
                                 "disaster.symptoms = true\n"
                                 "disaster.surge = false\n"
                                 "disaster.crash_every = 30\ndisaster.reboot_min = 10\ndisaster.reboot_max = 30\n"
                                 "disaster.crash_lead_min = 10\ndisaster.crash_lead_max = 30\n"
                                 "disaster.burnout_every = 40\ndisaster.burnout_lead_min = 10\n"
                                 "disaster.burnout_lead_max = 30\n"
                                 "disaster.diskfail_every = 50\ndisaster.diskfail_lead_min = 10\n"
                                 "disaster.diskfail_lead_max = 30\n"
                                 "disaster.storm_every = 60\ndisaster.storm_move_min = 20\ndisaster.storm_move_max = 40\n"
                                 "disaster.strike_every_min = 10\ndisaster.strike_every_max = 20\n";

void test_archive_columns() {
    const char* dir = "nova_test_cols_run";
    CreateDirectoryA(dir, nullptr);
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kSubDisWorld);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    TextBuf err;
    CHECK(run.open_stats("nova_test_cols_run/stats.csv", 25, &err));
    CHECK(run.open_nodes("nova_test_cols_run/nodes.csv", &err));
    CHECK(run.open_replay("nova_test_cols_run/replay.log", 50, &err));
    run.run_until(200);
    const World& w = run.world();
    const u32 nodes = w.node_count();
    Array<double> occ;
    Array<u32> offline;
    for (u32 n = 0; n < nodes; ++n) {
        occ.push(w.node_soup(n).occupancy());
        offline.push(w.node_soup(n).offline_count());
    }
    run.finish();
    RunArchive ar;
    ar.set_life(0);
    ar.set_novelty(0);
    CHECK(ar.load(dir, &err));
    CHECK(ar.windows() == 8);
    ViewFrame f;
    u64 struck = 0;
    double symptoms = 0;
    for (u32 k = 0; k < ar.windows(); ++k) {
        ar.build(k, ViewRequest{}, &f);
        for (u32 n = 0; n < f.nodes; ++n) {
            struck += f.node[n].struck;
            symptoms += f.node[n].symptoms;
        }
    }
    ar.build(ar.windows() - 1, ViewRequest{}, &f);
    bool subs = true, occ_ok = true, off_ok = true, known = true;
    u32 levels_seen = 0;
    for (u32 n = 0; n < f.nodes; ++n) {
        const NodeView& v = f.node[n];
        for (u32 t = 0; t < kSubstrateTypes; ++t) {
            subs = subs && fabs(v.sub[t] - w.substrate_mult(n, t)) < 1e-6 && v.sub_level[t] == w.substrate_level(n, t);
            levels_seen |= 1u << v.sub_level[t];
        }
        occ_ok = occ_ok && fabs(v.occupancy - occ[n]) < 2e-6;
        off_ok = off_ok && v.cores_offline == offline[n];
        known = known && (v.known >> kColorSubstrate & 1) && (v.known >> kColorOccupancy & 1) &&
                (v.known >> kColorSymptoms & 1) && (v.known >> kColorOffline & 1);
    }
    CHECK(subs && occ_ok && off_ok && known);
    CHECK(levels_seen == 0x1F); // every level somewhere
    CHECK(struck > 0 && symptoms > 0);
    // The disasters' events reach the archive as strikes (X7).
    u32 strikes = 0;
    for (usize i = 0; i < f.events.size(); ++i)
        strikes += event_is_strike(f.events[i].kind);
    ViewFrame first;
    RunArchive ar2;
    ar2.set_life(0);
    ar2.set_novelty(0);
    CHECK(ar2.load(dir, &err));
    ar2.build(0, ViewRequest{}, &first);
    for (usize i = 0; i < first.events.size(); ++i)
        strikes += event_is_strike(first.events[i].kind);
    CHECK(strikes > 0);
    printf("  nodes.csv: substrates, occupancy and the disasters' columns read back for %u nodes; %llu strikes in "
           "the windows, %u strike events\n",
           nodes, static_cast<unsigned long long>(struck), strikes);
    const char* files[] = {"stats.csv", "nodes.csv", "replay.log"};
    remove_dir(dir, files, 3);

    // By name: a hand-written file with the columns shuffled and some missing.
    const char* dir2 = "nova_test_cols_hand";
    CreateDirectoryA(dir2, nullptr);
    const char* text = "struck,node,sub_gpu,tick_last,occupancy,sub_p_cores,sub_e_cores,sub_npu,sub_mem_fast,sub_mem_slow,"
                       "sub_disk_fast,sub_disk_slow,cores_offline,bodies,tick_first,symptom_ticks\n"
                       "3,0,2,99,0.25,0.1,0.5,1,1.5,2,1,1,7,40,0,50\n"
                       "0,1,0.5,99,0.75,1,1,1,1,1,1,1,0,9,0,0\n";
    CHECK(write_file("nova_test_cols_hand/nodes.csv", text, strlen(text)));
    RunArchive hand;
    hand.set_life(0);
    hand.set_novelty(0);
    CHECK(hand.load(dir2, &err));
    ViewFrame g;
    hand.build(0, ViewRequest{}, &g);
    CHECK(g.nodes == 2 && g.node[0].struck == 3 && g.node[0].cores_offline == 7 && g.node[0].bodies == 40);
    CHECK(g.node[0].sub[kSubGpu] == 2.0f && g.node[0].sub_level[kSubGpu] == 4 && g.node[0].sub_level[kSubP] == 0 &&
          g.node[0].sub_level[kSubE] == 1 && g.node[0].sub_level[kSubMemFast] == 3);
    CHECK(g.node[0].occupancy == 0.25f && g.node[1].occupancy == 0.75f && g.node[0].symptoms == 0.5f);
    CHECK(g.node[1].sub_level[kSubGpu] == 1 && g.node[1].struck == 0);
    remove("nova_test_cols_hand/nodes.csv");
    RemoveDirectoryA(dir2);
    // An old file (no later columns) reads as before: 1x, nothing known.
    const char* dir3 = "nova_test_cols_old";
    CreateDirectoryA(dir3, nullptr);
    const char* old = "tick_first,tick_last,node,bodies\n0,9,0,5\n";
    CHECK(write_file("nova_test_cols_old/nodes.csv", old, strlen(old)));
    RunArchive o;
    o.set_life(0);
    o.set_novelty(0);
    CHECK(o.load(dir3, &err));
    o.build(0, ViewRequest{}, &g);
    CHECK(g.nodes == 1 && g.node[0].bodies == 5 && g.node[0].sub_level[kSubP] == kSubstrateOne &&
          !(g.node[0].known >> kColorSubstrate & 1) && !(g.node[0].known >> kColorOccupancy & 1));
    remove("nova_test_cols_old/nodes.csv");
    RemoveDirectoryA(dir3);
}

// --- A5: the map of life ---------------------------------------------------------

// Three planted families: variants of three unrelated genomes, each variant
// one codon changed (a different one, or to a different op, for every variant).
void planted_genome(u32 family, u32 variant, TextBuf& out) {
    static const char* const ops[16] = {"NOP0", "NOP1", "ZERO", "INC",  "DEC",  "SHL",  "ALLOC", "COPY",
                                        "DJNZ", "SENSE", "ROTB", "PUSH", "POP", "DIVIDE", "JMP", "CALL"};
    u64 h = mix64(family * 1000003ull + 17);
    const u32 pos = variant % 37, shift = 1 + (variant / 37) % 15;
    for (u32 i = 0; i < 40; ++i) {
        h = mix64(h + i);
        u32 op = static_cast<u32>(h % 16);
        if (i == pos)
            op = (op + shift) % 16;
        if (i)
            out.append_char(' ');
        out.append(ops[op]);
    }
}

void test_life_map() {
    // Features: unit length; fewer than 3 codons give none.
    Array<double> d;
    d.resize(kLifeDims);
    CHECK(life_features("NOP0 NOP1 ZERO INC DEC", d.data()));
    double norm = 0;
    for (u32 j = 0; j < kLifeDims; ++j)
        norm += d[j] * d[j];
    CHECK(fabs(norm - 1.0) < 1e-12);
    CHECK(!life_features("NOP0 NOP1", d.data()));

    // The reference window: 120 genotypes (the tests' size), then more.
    const u32 kRef = 120;
    LifeMap a, b;
    a.set_reference(kRef);
    b.set_reference(kRef);
    Array<double> mean_at;
    Matrix axes_at;
    Array<float> x_at, y_at;
    for (u32 i = 0; i < 360; ++i) {
        TextBuf g;
        planted_genome(i % 3, i, g);
        const u64 geno = hash_bytes(g.view().p, g.size(), 1);
        char name[16];
        snprintf(name, sizeof name, "0040a%03u", i);
        a.add(10 * i, geno, (static_cast<u64>(i % 3) << 32) | 1, i % 7, 40, name, g.view());
        b.add(10 * i, geno, (static_cast<u64>(i % 3) << 32) | 1, i % 7, 40, name, g.view());
        if (i + 1 == kRef) { // the axes as fitted: final from here on
            CHECK(a.final() && a.reference() == kRef);
            mean_at.append(a.mean().data(), a.mean().size());
            axes_at.resize(a.axes().rows, a.axes().cols);
            memcpy(axes_at.v.data(), a.axes().v.data(), a.axes().v.size() * sizeof(double));
            for (usize k = 0; k < a.points().size(); ++k) {
                x_at.push(a.points()[k].x);
                y_at.push(a.points()[k].y);
            }
        }
        if (i == 30)
            CHECK(a.fitted() && !a.final()); // provisional on the way
    }
    // Stable across windows: later genotypes moved neither the axes nor anyone.
    CHECK(memcmp(mean_at.data(), a.mean().data(), mean_at.size() * sizeof(double)) == 0);
    CHECK(memcmp(axes_at.v.data(), a.axes().v.data(), axes_at.v.size() * sizeof(double)) == 0);
    bool still = true;
    for (usize k = 0; k < x_at.size(); ++k)
        still = still && a.points()[k].x == x_at[k] && a.points()[k].y == y_at[k];
    CHECK(still);
    // The same input gives the same points.
    CHECK(a.points().size() == b.points().size() && a.points().size() == 360 &&
          memcmp(a.points().data(), b.points().data(), a.points().size() * sizeof(LifePoint)) == 0);
    // The planted families sit apart: each family's spread is far smaller than
    // the distance between their centres.
    double cx[3] = {}, cy[3] = {}, spread[3] = {}, count[3] = {};
    for (usize k = 0; k < a.points().size(); ++k) {
        const u32 fam = static_cast<u32>(a.points()[k].lineage >> 32);
        cx[fam] += a.points()[k].x;
        cy[fam] += a.points()[k].y;
        count[fam] += 1;
    }
    for (u32 i = 0; i < 3; ++i) {
        cx[i] /= count[i];
        cy[i] /= count[i];
    }
    for (usize k = 0; k < a.points().size(); ++k) {
        const u32 fam = static_cast<u32>(a.points()[k].lineage >> 32);
        const double dx = a.points()[k].x - cx[fam], dy = a.points()[k].y - cy[fam];
        spread[fam] += sqrt(dx * dx + dy * dy) / count[fam];
    }
    double gap = 1e9, wide = 0;
    for (u32 i = 0; i < 3; ++i) {
        wide = spread[i] > wide ? spread[i] : wide;
        for (u32 j = i + 1; j < 3; ++j) {
            const double dist = sqrt((cx[i] - cx[j]) * (cx[i] - cx[j]) + (cy[i] - cy[j]) * (cy[i] - cy[j]));
            gap = dist < gap ? dist : gap;
        }
    }
    CHECK(gap > 4 * wide);
    CHECK(a.variance_share(0) > 0.2 && a.variance_share(0) >= a.variance_share(1));
    printf("  map of life: 3 planted families %.3f apart, each spread %.3f; PC1 %.0f%%, PC2 %.0f%%\n", gap, wide,
           100.0 * a.variance_share(0), 100.0 * a.variance_share(1));
    // A genotype saved again is a save, not a point.
    TextBuf g0;
    planted_genome(0, 0, g0);
    a.add(9999, hash_bytes(g0.view().p, g0.size(), 1), 1, 3, 40, "0040a000", g0.view());
    CHECK(a.points().size() == 360 && a.points()[0].saves == 2 && a.points()[0].last == 9999);
}

// --- the panels ------------------------------------------------------------------

struct Rig2 {
    Run run;
    ViewSession session;
    Viewer viewer;
    Canvas canvas;
    RunStatus st;
    ViewFrame f;

    bool start(const char* cfg, u32 ticks, u64 list_window = 50) {
        if (!make_run(run, cfg, 4))
            return false;
        run.keep_genebank(true);
        session.attach(&run, 10);
        session.set_novelty_list(list_window, 5);
        for (u32 t = 0; t < ticks; ++t) {
            run.step();
            session.after_step();
        }
        refresh();
        return canvas.resize(1400, 900, 1.0f);
    }
    void refresh() {
        session.build(viewer.request(), &f);
        viewer.take(f);
    }
    void draw(const UiInput& in) { viewer.frame(in, canvas, st); }
    void click(i32 x, i32 y) {
        UiInput in;
        in.mx = x;
        in.my = y;
        in.pressed = in.released = true;
        draw(in);
    }
};

u32 count_color(const Canvas& c, u32 color, i32 x0, i32 y0, i32 x1, i32 y1) {
    u32 n = 0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > c.width() ? c.width() : x1;
    y1 = y1 > c.height() ? c.height() : y1;
    for (i32 y = y0; y < y1; ++y)
        for (i32 x = x0; x < x1; ++x)
            n += (c.pixels()[y * c.width() + x] & 0xFFFFFF) == color;
    return n;
}

u32 distinct_colors(const Canvas& c) {
    u32 seen[96] = {}, kinds = 0;
    for (i32 i = 0; i < c.width() * c.height(); i += 29) {
        const u32 v = c.pixels()[i] & 0xFFFFFF;
        bool found = false;
        for (u32 k = 0; k < kinds && !found; ++k)
            found = seen[k] == v;
        if (!found && kinds < 96)
            seen[kinds++] = v;
    }
    return kinds;
}

// A headless render to PNG, checked and then removed (the renders for people
// come from nova view --render: docs/results/viewer.md).
bool png_ok(Canvas& c, const char* path) {
    c.flush();
    ByteWriter png;
    png_encode(c.pixels(), static_cast<u32>(c.width()), static_cast<u32>(c.height()), static_cast<u32>(c.width()), png);
    const bool ok = write_file(path, png.data(), png.size()) && png.size() > 1000;
    Array<u8> back;
    const bool read = read_file(path, &back) && back.size() == png.size();
    remove(path);
    return ok && read;
}

// The list selects into the genome browser, and its lineage with it.
void test_kinds_select() {
    TextBuf cfg;
    novelty_world(cfg);
    Rig2* r = new Rig2;
    CHECK(r->start(cfg.c_str(), 400));
    r->viewer.set_kinds(true);
    UiInput none;
    r->draw(none);
    CHECK(r->viewer.kinds() && r->viewer.pick_count() >= 5);
    // A click on the third row of the list selects that genotype.
    const usize k = 2 < r->viewer.pick_count() ? 2 : 0;
    const NoveltyPick want = r->viewer.pick(k);
    i32 x = 0, y = 0;
    CHECK(r->viewer.pick_screen(k, &x, &y));
    r->click(x, y);
    const usize sel = r->viewer.selected_save();
    CHECK(sel < r->viewer.save_count());
    if (sel < r->viewer.save_count()) {
        const GenomeEntry& e = r->viewer.save(sel);
        CHECK(e.genotype == want.genotype && e.lineage == want.lineage && strcmp(e.name, want.name) == 0);
        CHECK(e.surprise >= 0 && fabs(e.surprise - want.surprise) < 1e-3);
    }
    CHECK(r->viewer.selected_lineage() == want.lineage && r->viewer.request().genotype == want.genotype);
    // The phylogeny then shows its lineage; the list is a click away.
    r->draw(none);
    CHECK(png_ok(r->canvas, "nova_test_viewer_kinds.png"));
    CHECK(distinct_colors(r->canvas) >= 24);
    // A pick the browser doesn't hold any more joins it (from the list's own genome).
    Viewer* v = new Viewer;
    ViewFrame f;
    f.nodes = 1;
    f.node.resize(1);
    f.novelty = true;
    f.novelty_window = 100;
    NoveltyPick p;
    memset(&p, 0, sizeof p);
    p.window_first = 0;
    p.window_last = 99;
    p.rank = 1;
    p.surprise = 4.5;
    p.lineage = (3ull << 32) | 2;
    p.genotype = 0; // (an archive's: novelty_top.csv has no hash)
    p.node = 0;
    p.len = 3;
    snprintf(p.name, sizeof p.name, "0003aab");
    p.off = 0;
    f.pick_text.append("NOP0 INC DIVIDE", 15);
    p.n = 15;
    f.picks.push(p);
    v->take(f);
    CHECK(v->pick_count() == 1 && v->save_count() == 0);
    v->select_pick(0);
    CHECK(v->save_count() == 1 && v->selected_save() == 0 && strcmp(v->save(0).name, "0003aab") == 0 &&
          v->selected_lineage() == p.lineage);
    delete v;
    delete r;
}

// Every substrate type draws, each node in its level's colour.
void test_substrate_mode() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("substrate.enabled = true\n");
    Rig2* r = new Rig2;
    CHECK(r->start(cfg.c_str(), 30));
    const World& w = r->run.world();
    bool frame_ok = true;
    for (u32 n = 0; n < w.node_count(); ++n)
        for (u32 t = 0; t < kSubstrateTypes; ++t)
            frame_ok = frame_ok && r->viewer.current().node[n].sub_level[t] == w.substrate_level(n, t) &&
                       r->viewer.current().node[n].sub[t] == static_cast<float>(w.substrate_mult(n, t));
    CHECK(frame_ok);
    r->viewer.set_color_by(kColorSubstrate);
    UiInput none;
    u32 types_ok = 0, differ = 0;
    u32 prev[64] = {};
    for (u32 t = 0; t < kSubstrateTypes; ++t) {
        r->viewer.set_sub_type(t);
        r->draw(none);
        r->canvas.flush();
        u32 match = 0, shown = 0, changed = 0;
        for (u32 n = 0; n < r->viewer.current().nodes && n < 64; ++n) {
            i32 x = 0, y = 0;
            if (!r->viewer.node_screen(n, &x, &y))
                continue;
            ++shown;
            const u32 lvl = r->viewer.current().node[n].sub_level[t];
            match += (r->canvas.pixels()[y * r->canvas.width() + x] & 0xFFFFFF) == substrate_level_color(lvl);
            changed += t > 0 && prev[n] != lvl;
            prev[n] = lvl;
        }
        types_ok += shown > 10 && match * 10 >= shown * 9;
        differ += changed > 0;
    }
    CHECK(types_ok == kSubstrateTypes);
    CHECK(differ >= 5); // the types have maps of their own
    // T cycles the type; the popup's list has the eight.
    UiInput key;
    key.keys[0] = 0x54;
    key.nkeys = 1;
    r->viewer.set_sub_type(7);
    r->draw(key);
    CHECK(r->viewer.sub_type() == 0);
    r->draw(key);
    CHECK(r->viewer.sub_type() == 1);
    r->draw(none);
    CHECK(png_ok(r->canvas, "nova_test_viewer_substrate.png"));
    // Occupancy, symptoms and offline cores draw too.
    for (u32 m = kColorSubstrate; m < kColorCount; ++m) {
        r->viewer.set_color_by(m);
        r->draw(none);
    }
    delete r;
}

// A made-up world of four nodes for the panels.
void small_frame(ViewFrame& f, u64 tick) {
    f.nodes = 4;
    f.node.resize(4);
    const float xy[4][2] = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f}, {0.8f, 0.8f}};
    for (u32 n = 0; n < 4; ++n) {
        NodeView& v = f.node[n];
        memset(&v, 0, sizeof v);
        v.x = xy[n][0];
        v.y = xy[n][1];
        v.live = 1;
        v.health = 100;
        v.bodies = 10 + n;
        v.known = (1u << kColorCount) - 1;
        for (u32 t = 0; t < kSubstrateTypes; ++t) {
            v.sub[t] = 1;
            v.sub_level[t] = kSubstrateOne;
        }
    }
    f.tick = tick;
}

void add_event(ViewFrame& f, u64 tick, const char* text) {
    ViewEvent e = {};
    e.tick = tick;
    parse_event(text, &e.kind, &e.node);
    e.off = static_cast<u32>(f.event_text.size());
    e.len = static_cast<u32>(strlen(text));
    f.event_text.append(text, e.len);
    f.events.push(e);
}

// X7: a disaster's marker appears on its node and fades.
void test_strike_marker() {
    // The disasters' events, as run.cpp logs them, parse to their kinds and nodes.
    u32 kind = 0, node = 0;
    parse_event("surge node=5 hops=2 nodes=7 deaths=30 bang=1", &kind, &node);
    CHECK(kind == kEvSurge && node == 5);
    parse_event("crash node=6 deleted=3 restarted=4 reboot=20", &kind, &node);
    CHECK(kind == kEvCrash && node == 6);
    parse_event("burnout node=1 cores=4 dormant=1 graced=0 died=0 repair=300", &kind, &node);
    CHECK(kind == kEvBurnout && node == 1);
    parse_event("diskfail node=2 codons=100 tier=fast", &kind, &node);
    CHECK(kind == kEvDiskFail && node == 2);
    parse_event("lightning node=3 codons=128 thunder=2048", &kind, &node);
    CHECK(kind == kEvLightning && node == 3);
    parse_event("storm moved from=1 to=2", &kind, &node);
    CHECK(kind == kEvOther && !event_is_strike(kind));

    Viewer* v = new Viewer;
    Canvas* c = new Canvas;
    CHECK(c->resize(1400, 900, 1.0f));
    RunStatus st;
    UiInput none;
    ViewFrame f;
    small_frame(f, 1000);
    add_event(f, 1000, "lightning node=2 codons=128 thunder=2048");
    add_event(f, 1000, "burnout node=1 cores=4 dormant=1 graced=0 died=0 repair=300");
    v->take(f);
    double age = 1;
    CHECK(v->strike_marker(2, &kind, &age) && kind == kEvLightning && age == 0);
    CHECK(v->strike_marker(1, &kind, &age) && kind == kEvBurnout);
    CHECK(!v->strike_marker(0, &kind, &age));
    v->frame(none, *c, st);
    c->flush();
    i32 x = 0, y = 0;
    CHECK(v->node_screen(2, &x, &y));
    const u32 fresh = count_color(*c, strike_color(kEvLightning), x - 40, y - 40, x + 40, y + 40);
    CHECK(fresh > 20);
    CHECK(png_ok(*c, "nova_test_viewer_strikes.png"));
    // Half way: still there, fainter.
    ViewFrame g;
    small_frame(g, 1000 + kStrikeFade / 2);
    v->take(g);
    CHECK(v->strike_marker(2, &kind, &age) && age > 0.49 && age < 0.51);
    v->frame(none, *c, st);
    c->flush();
    CHECK(count_color(*c, strike_color(kEvLightning), x - 40, y - 40, x + 40, y + 40) == 0); // (mixed toward the panel)
    CHECK(count_color(*c, mix_color(strike_color(kEvLightning), kPanel, age * 0.85), x - 40, y - 40, x + 40, y + 40) > 20);
    // Gone after kStrikeFade ticks.
    ViewFrame h;
    small_frame(h, 1000 + kStrikeFade);
    v->take(h);
    CHECK(!v->strike_marker(2, &kind, &age) && !v->strike_marker(1, &kind, &age));
    v->frame(none, *c, st);
    c->flush();
    u32 left = 0;
    for (u32 k = 0; k <= 2; ++k)
        left += count_color(*c, mix_color(strike_color(kEvLightning), kPanel, k * 0.4), x - 40, y - 40, x + 40, y + 40);
    CHECK(left == 0);
    delete c;
    delete v;
}

// W3: the fast region in the memory strip, drawn from the frame's accessor
// (view_fast_end returns the actual per-node boundary).
void test_fast_region() {
    Viewer* v = new Viewer;
    Canvas* c = new Canvas;
    CHECK(c->resize(1400, 900, 1.0f));
    RunStatus st;
    UiInput none;
    v->select_node(0);
    for (u32 pass = 0; pass < 2; ++pass) {
        ViewFrame f;
        small_frame(f, 10);
        f.sel_node = 0;
        f.strip_w = 256;
        f.strip_h = 16;
        f.strip.resize(256 * 16);
        f.strip.fill(0x303030);
        f.strip_owner.resize(256 * 16);
        f.strip_owner.fill(kNone);
        f.fast_end = pass ? 600 : 0;
        v->take(f);
        v->frame(none, *c, st);
        c->flush();
        const u32 n = count_color(*c, kFastMem, 0, 0, c->width(), c->height());
        CHECK(pass ? n > 200 : n == 0);
        if (pass)
            CHECK(png_ok(*c, "nova_test_viewer_fast.png"));
    }
    // The live accessor reports no region with tiers off, and the real boundary with them on.
    Run run;
    CHECK(make_run(run, kSoupWorldProc, 1));
    CHECK(view_fast_end(run.world().node_soup(0)) == 0);
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("mem.fast_share = 0.125\n");
    Run tiered;
    CHECK(make_run(tiered, cfg.c_str(), 1));
    const Soup& soup = tiered.world().node_soup(0);
    CHECK(view_fast_end(soup) == soup.fast_end() && soup.fast_end() > 0);
    ViewFrame live;
    ViewRequest request;
    request.node = 0;
    ViewSession session;
    session.attach(&tiered, 10);
    session.build(request, &live);
    CHECK(live.fast_end == soup.fast_end());
    delete c;
    delete v;
}

// The charts' series picker is the column list: every stats column is in it,
// the new ones too, and picking one plots it.
void test_series_picker() {
    TextBuf cfg;
    novelty_world(cfg);
    Rig2* r = new Rig2;
    CHECK(r->start(cfg.c_str(), 200));
    UiInput none;
    r->draw(none);
    r->viewer.open_series_picker(1);
    r->draw(none);
    CHECK(r->viewer.popup_kind() == 3 && r->viewer.popup_items() == kStatsColumns);
    const char* fresh[] = {"archive_held", "archive_stores", "founded_spark", "founded_wake", "surges", "strikes",
                           "episodes_active", "novelty", "novel_share", "novel_success", "compress", "ambient"};
    u32 visible = 0;
    for (const char* name : fresh) {
        Rect ir;
        visible += r->viewer.popup_item_rect(stats_column_index(name), &ir) && ir.right() <= r->canvas.width() &&
                   ir.bottom() <= r->canvas.height();
    }
    CHECK(visible == sizeof fresh / sizeof fresh[0]); // every one shown at once, no scrolling
    Rect ir;
    const u32 nov = stats_column_index("novelty");
    CHECK(r->viewer.popup_item_rect(nov, &ir));
    r->click(ir.x + ir.w / 2, ir.y + ir.h / 2);
    bool picked = false;
    for (u32 s = 0; s < 4; ++s)
        picked = picked || r->viewer.series(1, s) == nov;
    CHECK(picked);
    // The charts show novelty in bits per codon (not stats.csv's fixed point).
    double top = 0;
    for (usize i = 0; i < r->viewer.charts().size(); ++i)
        top = r->viewer.charts().value(i, nov) > top ? r->viewer.charts().value(i, nov) : top;
    CHECK(top > 0.5 && top < 20);
    delete r;
}

// The map of life in the viewer: the panel draws, points keep their place
// whatever window is shown, and a render is a picture.
void test_life_panel() {
    TextBuf cfg;
    novelty_world(cfg);
    Rig2* r = new Rig2;
    CHECK(r->start(cfg.c_str(), 400));
    r->viewer.set_life_mode(true);
    UiInput none;
    r->draw(none);
    const LifeMap& L = r->viewer.life();
    CHECK(L.fitted() && L.points().size() > 8);
    // A point seen in two windows sits in the same place in both.
    const u64 t0 = L.first_tick(), t1 = L.last_tick();
    r->viewer.set_life_time(t1, t1 - t0 + 1); // everything
    r->draw(none);
    u32 pick = kNone;
    for (u32 i = 0; i < L.points().size() && pick == kNone; ++i)
        if (L.points()[i].last >= (t0 + t1) / 2 && L.points()[i].first <= (t0 + t1) / 2)
            pick = i;
    if (pick == kNone)
        pick = 0;
    i32 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    CHECK(r->viewer.life_point_screen(pick, &x1, &y1));
    const LifePoint p = L.points()[pick];
    r->viewer.set_life_time(p.last, p.last - p.first + 10);
    r->draw(none);
    CHECK(r->viewer.life_point_screen(pick, &x2, &y2));
    CHECK(x1 == x2 && y1 == y2);
    // Points outside the window aren't drawn.
    r->viewer.set_life_time(t0, 1);
    r->draw(none);
    u32 shown = 0;
    for (u32 i = 0; i < L.points().size(); ++i) {
        i32 x, y;
        shown += r->viewer.life_point_screen(i, &x, &y);
    }
    CHECK(shown < L.points().size());
    r->viewer.set_life_follow();
    r->draw(none);
    CHECK(distinct_colors(r->canvas) >= 16);
    CHECK(png_ok(r->canvas, "nova_test_viewer_life.png"));
    // Live it keeps up: more saves become points.
    const usize before = L.points().size();
    for (u32 t = 0; t < 100; ++t) {
        r->run.step();
        r->session.after_step();
    }
    r->refresh();
    CHECK(r->viewer.life().points().size() >= before);
    CHECK(r->viewer.request().populations);
    r->refresh(); // (the frame carries living members: the panel sizes points by them)
    r->draw(none);
    delete r;
}

} // namespace

void viewer_more_view_tests() {
    test_novelty_columns();
    test_novelty_hash();
    test_live_picks();
    test_archive_columns();
    test_life_map();
}

void viewer_more_viewer_tests() {
    test_kinds_select();
    test_substrate_mode();
    test_strike_marker();
    test_fast_region();
    test_series_picker();
    test_life_panel();
}
