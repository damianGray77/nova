// Phase 6 (design.md 6.12): the analysis UI.
//
//   view    the testable core: the PNG writer, colours, chart samples, the
//           phylogeny layout, the view frame against the world it shows,
//           watching changing nothing (knobs included, replayed), the new
//           live knobs, numbered snapshots, the genebank log and a finished
//           run's files.
//   viewer  the panels, headless: made-up clicks and keys select, open and
//           command what they should, every colour mode draws, and a frame
//           renders to a PNG.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/png.h"
#include "nova/snapshot.h"
#include "nova/stats.h"
#include "nova/view.h"
#include "viewer.h"

using namespace nova;

namespace {

// kSoupWorldProc with every Phase 4 feature on: dispersal, signals, link
// failures, the lifecycle, so frames carry cargo, down links and fallow sites.
void proc_all(TextBuf& t) {
    t.append(kSoupWorldProc);
    t.append(kPhase4All);
}

u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
}

// --- the PNG writer ------------------------------------------------------------------

// Walks a PNG: checks every chunk's CRC, then rebuilds the raw scanlines from
// the stored deflate blocks and checks the Adler-32. Returns the raw bytes.
bool png_decode_stored(const ByteWriter& png, u32* w, u32* h, Array<u8>* raw) {
    const u8* p = png.data();
    const usize n = png.size();
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8 || memcmp(p, sig, 8) != 0)
        return false;
    usize at = 8;
    Array<u8> z;
    bool end = false;
    while (at + 12 <= n && !end) {
        const u32 len = be32(p + at);
        if (at + 12 + len > n)
            return false;
        const u8* type = p + at + 4;
        const u8* data = p + at + 8;
        if (crc32(type, 4 + len) != be32(data + len))
            return false;
        if (memcmp(type, "IHDR", 4) == 0) {
            *w = be32(data);
            *h = be32(data + 4);
            if (len != 13 || data[8] != 8 || data[9] != 2)
                return false;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            z.append(data, len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            end = len == 0 && be32(data) == 0xAE426082u;
        }
        at += 12 + len;
    }
    if (!end || at != n || z.size() < 6 || z[0] != 0x78 || ((z[0] << 8) | z[1]) % 31 != 0)
        return false;
    raw->clear();
    usize k = 2;
    for (;;) {
        if (k + 5 > z.size())
            return false;
        const bool last = z[k] & 1;
        const u32 blen = z[k + 1] | (z[k + 2] << 8);
        const u32 nlen = z[k + 3] | (z[k + 4] << 8);
        if ((z[k] & 6) != 0 || (blen ^ nlen) != 0xFFFF || k + 5 + blen > z.size())
            return false;
        raw->append(z.data() + k + 5, blen);
        k += 5 + blen;
        if (last)
            break;
    }
    return k + 4 == z.size() && be32(z.data() + k) == adler32(raw->data(), raw->size());
}

void test_png() {
    // Known checksums.
    CHECK_EQ_U64(crc32(reinterpret_cast<const u8*>("123456789"), 9), 0xCBF43926u);
    CHECK_EQ_U64(adler32(reinterpret_cast<const u8*>("Wikipedia"), 9), 0x11E60398u);
    CHECK_EQ_U64(crc32(reinterpret_cast<const u8*>("IEND"), 4), 0xAE426082u);

    // A 3 x 2 image, byte for byte: 88 bytes, one stored block.
    const u32 px[6] = {0xFF0000, 0x00FF00, 0x0000FF, 0x123456, 0xABCDEF, 0xFFFFFF};
    ByteWriter png;
    png_encode(px, 3, 2, 3, png);
    CHECK(png.size() == 88);
    u32 w = 0, h = 0;
    Array<u8> raw;
    CHECK(png_decode_stored(png, &w, &h, &raw));
    CHECK(w == 3 && h == 2);
    const u8 want[20] = {0, 0xFF, 0, 0, 0, 0xFF, 0, 0, 0, 0xFF, 0, 0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF, 0xFF, 0xFF, 0xFF};
    CHECK(raw.size() == 20 && memcmp(raw.data(), want, 20) == 0);
    // The stored block's header right after the zlib header: final, 20 bytes.
    const u8* idat = png.data() + 8 + 25 + 8;
    CHECK(idat[0] == 0x78 && idat[1] == 0x01 && idat[2] == 1 && idat[3] == 20 && idat[4] == 0 && idat[5] == 0xEB &&
          idat[6] == 0xFF);

    // A bigger one spans several stored blocks (the top byte of a pixel is
    // ignored), and a stride wider than the image skips the padding.
    Array<u32> big;
    big.resize(210 * 200);
    for (u32 i = 0; i < big.size(); ++i)
        big[i] = (i * 2654435761u) | 0xFF000000u;
    ByteWriter png2;
    png_encode(big.data(), 200, 200, 210, png2);
    CHECK(png_decode_stored(png2, &w, &h, &raw));
    CHECK(w == 200 && h == 200 && raw.size() == 200u * 601u);
    bool same = true;
    for (u32 y = 0; y < 200 && same; ++y) {
        same = raw[y * 601u] == 0;
        for (u32 x = 0; x < 200 && same; ++x) {
            const u32 v = big[y * 210 + x];
            const u8* q = raw.data() + y * 601u + 1 + 3 * x;
            same = q[0] == ((v >> 16) & 255) && q[1] == ((v >> 8) & 255) && q[2] == (v & 255);
        }
    }
    CHECK(same);
}

// --- colours, charts, the phylogeny ---------------------------------------------------

void test_colours() {
    CHECK(lineage_color(0) == 0x808080);
    CHECK(lineage_color((3ull << 32) | 7) == lineage_color((3ull << 32) | 7)); // stable
    u32 distinct = 0;
    for (u64 id = 1; id <= 64; ++id)
        distinct += lineage_color(id) != lineage_color(id + 1);
    CHECK(distinct >= 60);
    CHECK(ramp_color(0) == 0x440154 && ramp_color(1) == 0xFDE725 && ramp_color(-3) == 0x440154);
    CHECK(heat_color(1) == 0xFFFFFF);
    CHECK(mix_color(0x000000, 0xFFFFFF, 0.5) == 0x808080);
}

void test_chart_log() {
    ChartLog log;
    double row[kStatsColumns] = {};
    for (u64 t = 1; t <= 10000; ++t) {
        row[0] = static_cast<double>(t);
        log.add(t * 10, row);
    }
    CHECK(log.size() < ChartLog::kChartPoints && log.size() > ChartLog::kChartPoints / 2);
    CHECK(log.step() == 4);
    bool ok = log.tick(0) == 10;
    for (usize i = 1; i < log.size() && ok; ++i)
        ok = log.tick(i) == log.tick(i - 1) + 10 * log.step() && log.value(i, 0) * 10 == log.tick(i);
    CHECK(ok); // evenly spaced at the current step, values with their ticks

    // Sums become per-tick rates; state columns stay as they are.
    StatsRow r = {};
    r.v[2] = 4;  // ticks
    r.v[4] = 20; // births
    r.v[42] = 7; // bodies
    r.compress = 0.5;
    double out[kStatsColumns];
    chart_row(r, out);
    CHECK(out[4] == 5 && out[42] == 7 && out[kStatsIntColumns] == 0.5);
    CHECK(stats_column_index("bodies") == 42 && stats_column_index("compress") == kStatsIntColumns &&
          stats_column_index("nope") == kStatsColumns);
}

void test_stats_header() {
    // The shared column table writes exactly the header stats.csv always had.
    const char* path = "nova_test_view_header.csv";
    Run run;
    CHECK(make_run(run, kSoupWorld1b, 1));
    TextBuf err;
    CHECK(run.open_stats(path, 5, &err));
    run.run_until(5);
    run.finish();
    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    const char* header = "tick_first,tick_last,ticks,instructions,births,deaths,sparks,halts,"
                         "copy_errors,flaws,rot_flips,cpu_released,mem_released,engulf_tries,engulfs,"
                         "inject_tries,injects,upkeep,cpu_worn,gpu_elements,gpu_released,gpu_worn,"
                         "npu_macs,npu_released,npu_worn,sent,arrived,lost,corrupted,transit_lost,"
                         "disk_stores,disk_loads,disk_compares,disk_rot,wipes,wiped,wipe_deaths,emits,"
                         "listens,collapses,recoveries,garbled,bodies,cpus,free_millicores,"
                         "waste,gpu_held,npu_held,in_flight,links,inject_links,gpu_links,max_depth,"
                         "lineages,top_lineage,genotypes_in_top,colonised,links_down,disk_held,live_nodes,"
                         "warp_steps,lane_instr,warp_host,launches,gpu_births,warps,gpu_bodies,"
                         "actions_started,actions_landed,actions_failed,actions_defended,"
                         "compress,ambient\n";
    CHECK(bytes.size() > strlen(header) && memcmp(bytes.data(), header, strlen(header)) == 0);
    remove(path);
}

void test_phylo() {
    // A (root) -> B, C; B -> D; E <-> F a cycle; G another lineage.
    GenomeEntry e[8];
    memset(e, 0, sizeof e);
    const u64 L = (2ull << 32) | 1;
    const u64 geno[8] = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x77, 0xB};
    const u64 parent[8] = {0, 0xA, 0xA, 0xB, 0xF, 0xE, 0, 0xA};
    for (u32 i = 0; i < 8; ++i) {
        e[i].tick = 10 * (i + 1);
        e[i].genotype = geno[i];
        e[i].parent = parent[i];
        e[i].lineage = i == 6 ? L + 1 : L;
    }
    Array<PhyloNode> out;
    phylo_layout(e, 8, L, &out);
    CHECK(out.size() == 6); // G is another lineage; the second B is the same genotype
    // Preorder: A, B, D, C, then the cycle's first member E and its child F.
    const u32 order[6] = {0, 1, 3, 2, 4, 5};
    bool rows = true;
    for (u32 r = 0; r < 6; ++r) {
        bool found = false;
        for (usize i = 0; i < out.size(); ++i)
            if (out[i].entry == order[r])
                found = out[i].row == r;
        rows = rows && found;
    }
    CHECK(rows);
    for (usize i = 0; i < out.size(); ++i) {
        const u32 want_depth[6] = {0, 1, 1, 2, 0, 1};
        CHECK(out[i].depth == want_depth[out[i].entry]);
    }
    phylo_layout(e, 8, 12345, &out);
    CHECK(out.empty());
}

// --- the live session ------------------------------------------------------------------

// The busiest node: a good inspector target.
u32 busiest(const World& w) {
    u32 best = 0;
    for (u32 n = 1; n < w.node_count(); ++n)
        if (w.node_soup(n).body_count() > w.node_soup(best).body_count())
            best = n;
    return best;
}

void test_frame_matches_world() {
    TextBuf cfg;
    proc_all(cfg);
    Run run;
    CHECK(make_run(run, cfg.c_str(), 4));
    run.keep_genebank(true);
    ViewSession s;
    s.attach(&run, 10);
    for (u32 t = 0; t < 120; ++t) {
        run.step();
        s.after_step();
    }
    const World& w = run.world();
    ViewRequest q;
    q.node = busiest(w);
    ViewFrame f;
    const usize saved = run.genebank_count();
    s.build(q, &f);
    CHECK(f.tick == 120 && f.nodes == w.node_count() && f.node.size() == w.node_count() && f.procedural);
    bool nodes_ok = true;
    for (u32 n = 0; n < w.node_count(); ++n) {
        const NodeView& v = f.node[n];
        nodes_ok = nodes_ok && v.bodies == w.node_soup(n).body_count() && v.live == (w.live(n) ? 1u : 0u) &&
                   v.x >= 0 && v.x <= 1 && v.y >= 0 && v.y <= 1 && v.soup_size == w.node_params(n).size;
    }
    CHECK(nodes_ok);
    u32 undirected = 0;
    for (u32 e = 0; e < w.edge_count(); ++e)
        undirected += w.edge_src(e) < w.edge_dst(e);
    CHECK(f.edge.size() == undirected);
    CHECK(f.cargo.size() == w.in_flight());
    bool cargo_ok = true;
    for (usize i = 0; i < f.cargo.size(); ++i)
        cargo_ok = cargo_ok && f.cargo[i].x >= 0 && f.cargo[i].x <= 1 && f.cargo[i].y >= 0 && f.cargo[i].y <= 1;
    CHECK(cargo_ok);
    CHECK(f.knobs == 7 && strcmp(f.knob_key[0], "spark.rate") == 0 && strcmp(f.knob_key[6], "edge.corruption") == 0);

    // The inspector's node: one strip pixel per codon, owners as the soup says.
    const Soup& soup = w.node_soup(q.node);
    CHECK(f.sel_node == q.node && f.strip_w * f.strip_h >= soup.size() && f.strip_owner.size() == f.strip.size());
    bool owners = true;
    for (u32 a = 0; a < soup.size() && owners; ++a)
        owners = f.strip_owner[a] == soup.owner_body(a);
    CHECK(owners);
    CHECK(f.bodies.size() == soup.body_count() && !f.bodies.empty());
    // Chart samples, events and saves come once.
    CHECK(f.sample_tick.size() == 12 && f.sample.size() == 12 * kStatsColumns && f.sample_tick[0] == 10);
    CHECK(f.events.size() == run.event_count());
    CHECK(f.saves.size() == saved && saved > 0);
    CHECK(run.genebank_count() == 0); // handed over: the run's log doesn't grow for hours
    if (!f.saves.empty())
        CHECK(f.saves[0].name[0] >= '0' && f.saves[0].name[0] <= '9' && f.saves[0].n > 0);

    // A body, then the same slot with another serial (a reused slot).
    const BodyRow b = f.bodies[0];
    q.body = b.slot;
    q.serial = b.serial;
    for (usize i = 0; i < f.bodies.size(); ++i)
        if (f.bodies[i].lineage)
            q.genotype = f.bodies[i].genotype;
    s.build(q, &f);
    CHECK(f.body_ok && f.sel_body == b.slot && f.cpus.size() == soup.body(b.slot).ncpu && !f.genome.empty());
    CHECK(f.sample_tick.empty() && f.events.empty() && f.saves.empty()); // nothing new
    u32 holding = 0;
    for (u32 n = 0; n < f.nodes; ++n)
        holding += f.node[n].holding;
    CHECK(q.genotype == 0 || holding >= 1);
    q.serial = b.serial + 1;
    s.build(q, &f);
    CHECK(!f.body_ok && f.req_body == b.slot);

    // Every strip colouring runs.
    for (u32 m = 0; m < kStripCount; ++m) {
        q.strip = m;
        s.build(q, &f);
        CHECK(f.strip.size() >= soup.size());
    }
}

// Watching changes nothing: a run whose every tick is followed by a frame
// (with a changing selection) and whose knobs come from a pretend UI at the
// current tick hashes exactly as the same run with the same knobs scheduled
// up front, and its replay log replays.
void test_watching_changes_nothing() {
    TextBuf cfg;
    proc_all(cfg);
    struct K {
        u64 tick;
        const char* key;
        const char* value;
    };
    const K knobs[] = {{50, "spark.rate", "0.8"},
                       {100, "cpu.wear", "0.004"},
                       {150, "heat.ambient", "0.1"},
                       {200, "edge.corruption", "0.02"},
                       {220, "spark.grant", "30"}};
    const u32 nk = sizeof knobs / sizeof knobs[0];
    TextBuf err;

    Run plain;
    CHECK(make_run(plain, cfg.c_str(), 4));
    for (u32 i = 0; i < nk; ++i)
        CHECK(plain.schedule_knob(knobs[i].tick, knobs[i].key, knobs[i].value, &err));
    plain.run_until(300);
    const u64 want = plain.world().hash();

    const char* log_path = "nova_test_view_watch.replay";
    Run watched;
    CHECK(make_run(watched, cfg.c_str(), 20));
    watched.keep_genebank(true);
    CHECK(watched.open_replay(log_path, 50, &err));
    ViewSession s;
    s.attach(&watched, 7);
    ViewFrame f;
    u32 next = 0;
    while (watched.world().tick() < 300) {
        const u64 t = watched.world().tick();
        if (next < nk && knobs[next].tick == t) {
            CHECK(watched.schedule_knob(t, knobs[next].key, knobs[next].value, &err));
            ++next;
        }
        watched.step();
        s.after_step();
        ViewRequest q;
        q.node = static_cast<u32>(t % watched.world().node_count());
        q.strip = static_cast<u32>(t % kStripCount);
        q.hash = t % 50 == 0;
        if (t % 3 == 0 && !f.bodies.empty()) {
            q.body = f.bodies[0].slot;
            q.serial = f.bodies[0].serial;
            q.genotype = f.bodies[0].genotype;
        }
        s.build(q, &f);
    }
    CHECK_EQ_U64(watched.world().hash(), want);
    watched.finish();

    ReplayLog log;
    CHECK(log.load_file(log_path, &err));
    CHECK(log.knob_count() == nk);
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r));
    CHECK(r.ok);

    // The knobs mattered.
    Run bare;
    CHECK(make_run(bare, cfg.c_str(), 4));
    bare.run_until(300);
    CHECK(bare.world().hash() != want);

    // A snapshot carries the knobs' values in its config and the world's.
    ByteWriter snap;
    watched.snapshot(snap);
    Run again;
    CHECK(again.init_from_snapshot(snap.data(), snap.size(), 4, &err));
    CHECK(again.config().get_str("cpu.wear", "").eq("0.004") && again.config().get_str("heat.ambient", "").eq("0.1"));
    CHECK(again.world().knob_now(4) == 0.004 && again.world().knob_now(6) == 0.02 &&
          again.world().knob_now(1) == 30);
    remove(log_path);
}

void test_live_knobs() {
    Run nodisp;
    CHECK(make_run(nodisp, kSoupWorld1b, 1));
    const World& w = nodisp.world();
    CHECK(w.knob_count() == 6);
    CHECK(w.check_knob("edge.corruption", "0.1") == KnobResult::not_live); // no dispersal, no cargo
    CHECK(w.check_knob("cpu.wear", "2") == KnobResult::bad_value);
    CHECK(w.check_knob("heat.ambient", "-1") == KnobResult::bad_value);
    CHECK(w.check_knob("cpu.wear", "0.01") == KnobResult::ok);
    CHECK(w.check_knob("heat.cool", "0.5") == KnobResult::not_live); // still structural
    TextBuf err;
    CHECK(nodisp.schedule_knob(0, "cpu.wear", "0.01", &err));
    CHECK(nodisp.schedule_knob(0, "heat.ambient", "0.2", &err));
    nodisp.step();
    CHECK(w.knob_now(4) == 0.01 && w.node_params(3).cpu_wear == 0.01);
    CHECK(w.node_params(5).ambient == 0.2f && w.knob_now(5) == static_cast<double>(0.2f));
    CHECK(nodisp.config().get_str("cpu.wear", "").eq("0.01"));
}

void test_numbered_snapshots() {
    const char* base = "nova_test_view_snap";
    Run run;
    CHECK(make_run(run, kSoupWorldProc, 4));
    run.open_snapshots(base, 40);
    run.run_until(80);
    CHECK(run.snapshot_error().n == 0);
    const u64 want = run.world().hash();
    Array<u8> bytes;
    CHECK(read_file("nova_test_view_snap.40", &bytes));
    Run resumed;
    TextBuf err;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 2, &err));
    CHECK(resumed.world().tick() == 40);
    resumed.run_until(80);
    CHECK_EQ_U64(resumed.world().hash(), want);
    Array<u8> b80;
    CHECK(read_file("nova_test_view_snap.80", &b80));
    remove("nova_test_view_snap.40");
    remove("nova_test_view_snap.80");
    // A path that can't be written is reported, not fatal.
    Run bad;
    CHECK(make_run(bad, kSoupWorld1b, 1));
    bad.open_snapshots("nova_no_such_dir/x/snap", 1);
    bad.step();
    CHECK(bad.snapshot_error().n > 0);
}

void test_genebank_log() {
    const char* path = "nova_test_view_genebank.txt";
    Run run;
    CHECK(make_run(run, kSoupWorld1b, 4));
    run.keep_genebank(true);
    TextBuf err;
    CHECK(run.open_genebank(path, &err));
    run.run_until(400);
    run.finish();
    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    usize entries = 0;
    for (usize i = 0; i < bytes.size(); ++i)
        if ((i == 0 || bytes[i - 1] == '\n') && bytes[i] != '#' && bytes[i] != ' ' && bytes[i] != '\n')
            ++entries;
    CHECK(entries > 0 && entries == run.genebank_count());
    bool ok = true;
    for (usize i = 0; i < run.genebank_count() && ok; ++i) {
        const GenebankRecord& r = run.genebank_record(i);
        ok = r.len > 0 && genotype_hash(run.genebank_codons() + r.codon_off, r.len) == r.genotype;
    }
    CHECK(ok);
    remove(path);
}

// A finished run's files give the same numbers back.
void test_archive() {
    const char* dir = "nova_test_view_run";
    CreateDirectoryA(dir, nullptr);
    Run run;
    CHECK(make_run(run, kSoupWorldProc, 4));
    run.keep_genebank(true);
    TextBuf err;
    CHECK(run.open_stats("nova_test_view_run/stats.csv", 25, &err));
    CHECK(run.open_nodes("nova_test_view_run/nodes.csv", &err));
    CHECK(run.open_replay("nova_test_view_run/replay.log", 50, &err));
    CHECK(run.open_genebank("nova_test_view_run/genebank.txt", &err));
    run.run_until(150);
    const World& w = run.world();
    u32 bodies_at_end[64] = {};
    for (u32 n = 0; n < w.node_count() && n < 64; ++n)
        bodies_at_end[n] = static_cast<u32>(w.node_soup(n).body_count());
    run.finish();

    RunArchive ar;
    CHECK(ar.load(dir, &err));
    CHECK(ar.windows() == 6 && ar.window_tick(0) == 25 && ar.window_tick(5) == 150);
    ViewFrame f;
    ViewRequest q;
    ar.build(5, q, &f);
    CHECK(f.archive && f.nodes == w.node_count() && f.tick == 150);
    bool same = true;
    for (u32 n = 0; n < f.nodes && n < 64; ++n)
        same = same && f.node[n].bodies == bodies_at_end[n] && f.node[n].x == static_cast<float>(w.node_x(n));
    CHECK(same);
    u32 undirected = 0;
    for (u32 e = 0; e < w.edge_count(); ++e)
        undirected += w.edge_src(e) < w.edge_dst(e);
    CHECK(f.edge.size() == undirected);
    CHECK(f.sample_tick.size() == 6 && f.sample_tick[5] == 150);
    CHECK(f.events.size() == run.event_count());
    CHECK(f.saves.size() == run.genebank_count());
    bool names = run.genebank_count() > 0;
    for (usize i = 0; i < f.saves.size() && names; ++i) {
        const GenebankRecord& r = run.genebank_record(i);
        names = f.saves[i].genotype == r.genotype && f.saves[i].lineage == r.lineage && f.saves[i].len == r.len &&
                f.saves[i].tick == r.tick;
    }
    CHECK(names);
    ar.build(2, q, &f); // later builds bring nothing new
    CHECK(f.sample_tick.empty() && f.saves.empty() && f.tick == 75);

    remove("nova_test_view_run/stats.csv");
    remove("nova_test_view_run/nodes.csv");
    remove("nova_test_view_run/replay.log");
    remove("nova_test_view_run/genebank.txt");
    RemoveDirectoryA(dir);
    RunArchive none;
    TextBuf e2;
    CHECK(!none.load("nova_test_view_nothing_here", &e2) && e2.size() > 0);
}

// --- the panels ---------------------------------------------------------------------------

struct Rig {
    Run run;
    ViewSession session;
    Viewer viewer;
    Canvas canvas;
    RunStatus st;
    ViewFrame f;

    bool start(u32 ticks) {
        TextBuf cfg;
        proc_all(cfg);
        if (!make_run(run, cfg.c_str(), 4))
            return false;
        run.keep_genebank(true);
        session.attach(&run, 10);
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

void test_viewer_input() {
    Rig* r = new Rig;
    CHECK(r->start(150));
    UiInput none;
    r->draw(none);

    // Click a node on the map: it becomes the inspector's node, and the next
    // frame carries its memory strip and bodies.
    i32 x = 0, y = 0;
    CHECK(r->viewer.node_screen(3, &x, &y));
    r->click(x, y);
    CHECK(r->viewer.selected_node() == 3 && r->viewer.request().node == 3);
    r->refresh();
    CHECK(r->viewer.current().sel_node == 3 && r->viewer.current().strip_w == 256);
    r->draw(none);

    // Pause and Step in the toolbar become commands.
    r->viewer.commands().clear();
    r->click(40, 15); // Pause (8..84 x 4..30 at 96 dpi)
    CHECK(r->viewer.commands().size() == 1 && r->viewer.commands()[0].kind == kCmdPause);
    r->viewer.commands().clear();
    r->click(100, 15); // Step (88..140)
    CHECK(r->viewer.commands().size() == 1 && r->viewer.commands()[0].kind == kCmdStep);
    r->viewer.commands().clear();

    // Typing a knob's value: click its field, type, Enter.
    Canvas& c = r->canvas;
    const i32 fx = c.px(8) + c.px(52) + c.text_w("spark.rate", kFontSmall) + c.px(4) + 10;
    const i32 fy = c.px(4) + c.px(26) + c.px(6) + 10;
    r->click(fx, fy);
    CHECK(r->viewer.knob_editing());
    UiInput typing;
    typing.mx = fx;
    typing.my = fy;
    for (u32 i = 0; i < 3; ++i) // clear the current value
        typing.keys[typing.nkeys++] = 0x08;
    typing.chars[0] = '0';
    typing.chars[1] = '.';
    typing.chars[2] = '5';
    typing.nchars = 3;
    r->draw(typing);
    UiInput enter;
    enter.keys[0] = 0x0D;
    enter.nkeys = 1;
    r->draw(enter);
    CHECK(!r->viewer.knob_editing());
    CHECK(r->viewer.commands().size() == 1 && r->viewer.commands()[0].kind == kCmdKnob &&
          strcmp(r->viewer.commands()[0].key, "spark.rate") == 0 && strcmp(r->viewer.commands()[0].text, "0.5") == 0);
    r->viewer.commands().clear();
    // The command is applied between ticks, as the shell does it.
    TextBuf err;
    CHECK(r->run.schedule_knob(r->run.world().tick(), "spark.rate", "0.5", &err));
    r->run.step();
    CHECK(r->run.world().knob_now(0) == 0.5);

    // Every colour mode and strip mode draws; the wheel zooms the map.
    for (u32 m = 0; m < kColorCount; ++m) {
        r->viewer.set_color_by(m);
        r->draw(none);
    }
    UiInput wheel;
    wheel.mx = x;
    wheel.my = y;
    wheel.wheel = 2;
    r->draw(wheel);
    i32 x2 = 0, y2 = 0;
    CHECK(r->viewer.node_screen(3, &x2, &y2)); // zoomed about the cursor: the node stays put
    CHECK(x2 - x <= 1 && x - x2 <= 1 && y2 - y <= 1 && y - y2 <= 1);
    delete r;
}

void test_viewer_auto_and_render() {
    Rig* r = new Rig;
    CHECK(r->start(200));
    for (int i = 0; i < 4; ++i) {
        r->viewer.auto_select();
        r->refresh();
    }
    CHECK(r->viewer.selected_node() != kNone);
    CHECK(r->viewer.selected_body() != kNone);
    CHECK(r->viewer.save_count() > 0 && r->viewer.selected_save() < r->viewer.save_count());
    CHECK(r->viewer.charts().size() == 20);
    UiInput none;
    r->draw(none);
    // The browser's filter: by length, a range, a lineage, or nothing.
    const usize all = r->viewer.browser_rows();
    CHECK(all == r->viewer.save_count());
    const GenomeEntry& e0 = r->viewer.save(0);
    char text[32];
    snprintf(text, sizeof text, "%u", e0.len);
    r->viewer.set_filter(text);
    usize want = 0;
    for (usize i = 0; i < r->viewer.save_count(); ++i)
        want += r->viewer.save(i).len == e0.len;
    CHECK(r->viewer.browser_rows() == want && want >= 1);
    snprintf(text, sizeof text, "%u.%u", static_cast<unsigned>(e0.lineage >> 32),
             static_cast<unsigned>(e0.lineage & 0xFFFFFFFFu));
    r->viewer.set_filter(text);
    want = 0;
    for (usize i = 0; i < r->viewer.save_count(); ++i)
        want += r->viewer.save(i).lineage == e0.lineage;
    CHECK(r->viewer.browser_rows() == want);
    r->viewer.set_filter("0-1");
    CHECK(r->viewer.browser_rows() == 0);
    r->viewer.set_filter("");
    CHECK(r->viewer.browser_rows() == all);
    r->draw(none);
    r->canvas.flush();
    // The picture: a PNG of the right size with more than a few colours in it.
    ByteWriter png;
    png_encode(r->canvas.pixels(), 1400, 900, 1400, png);
    u32 w = 0, h = 0;
    Array<u8> raw;
    CHECK(png_decode_stored(png, &w, &h, &raw));
    CHECK(w == 1400 && h == 900);
    u32 seen[64] = {};
    u32 kinds = 0;
    for (u32 i = 0; i < 1400u * 900u; i += 37) {
        const u32 v = r->canvas.pixels()[i] & 0xFFFFFF;
        bool found = false;
        for (u32 k = 0; k < kinds && !found; ++k)
            found = seen[k] == v;
        if (!found && kinds < 64)
            seen[kinds++] = v;
    }
    CHECK(kinds >= 32);
    delete r;
}

// The viewer keeps the newest kMaxSaves saves: past that the oldest quarter
// goes, and a selection moves with its entry.
void test_viewer_log_cap() {
    Viewer* v = new Viewer;
    ViewFrame f;
    f.nodes = 1;
    f.node.resize(1);
    const usize n = kMaxSaves + 10;
    for (usize i = 0; i < n; ++i) {
        GenomeEntry e = {};
        e.tick = i;
        e.genotype = i + 1;
        e.lineage = 1;
        e.len = 3;
        e.off = static_cast<u32>(f.save_text.size());
        f.save_text.append("ABC", 3);
        e.n = 3;
        f.saves.push(e);
    }
    v->take(f);
    const usize dropped = n - kMaxSaves + kMaxSaves / 4;
    CHECK(v->save_count() == n - dropped);
    CHECK(v->save(0).tick == dropped && v->save(v->save_count() - 1).tick == n - 1);
    v->select_save(v->save_count() - 1);
    // One more batch past the cap again: the selection follows its entry.
    ViewFrame g;
    g.nodes = 1;
    g.node.resize(1);
    for (usize i = 0; i < kMaxSaves; ++i) {
        GenomeEntry e = {};
        e.tick = n + i;
        e.lineage = 1;
        e.off = static_cast<u32>(g.save_text.size());
        g.save_text.append("D", 1);
        e.n = 1;
        g.saves.push(e);
    }
    v->take(g);
    CHECK(v->save_count() <= kMaxSaves);
    CHECK(v->selected_save() == ~static_cast<usize>(0) || v->save(v->selected_save()).tick == n - 1);
    delete v;
}

// A landscape in the viewer (Phase 7): the land comes in the first frame only,
// the overview map moves the view, double-click fits a landmass.
void test_viewer_landscape() {
    const char* cfg = "seed = 3\n"
                      "world.model = soup\n"
                      "world.graph = landscape\n"
                      "world.nodes = 150\n"
                      "land.scale = 5\n"
                      "land.fraction = 0.3\n"
                      "land.resolution = 96\n"
                      "world.soup_size = 1024\n"
                      "world.millicores = 300\n"
                      "spark.initial = 10\n"
                      "spark.rate = 0.3\n"
                      "detect.enabled = true\n";
    Run run;
    CHECK(make_run(run, cfg, 4));
    ViewSession s;
    s.attach(&run, 10);
    for (u32 t = 0; t < 20; ++t) {
        run.step();
        s.after_step();
    }
    ViewFrame f;
    Viewer* v = new Viewer;
    s.build(v->request(), &f);
    CHECK(f.landscape && f.land_sent && f.land.size() == static_cast<usize>(f.land_w) * f.land_h && !f.masses.empty());
    v->take(f);
    s.build(v->request(), &f);
    CHECK(f.landscape && !f.land_sent && f.land.empty()); // once is enough
    bool bridges = false;
    for (usize i = 0; i < f.edge.size(); ++i)
        bridges = bridges || f.edge[i].bridge;
    CHECK(bridges);
    v->take(f);
    Canvas* c = new Canvas;
    CHECK(c->resize(1400, 900, 1.0f));
    RunStatus st;
    UiInput none;
    v->frame(none, *c, st);
    const Rect mm = v->minimap();
    CHECK(mm.w > 50 && mm.h > 20);
    // A click at the overview's left edge moves the view there.
    UiInput click;
    click.mx = mm.x + 2;
    click.my = mm.y + mm.h / 2;
    click.pressed = click.down = true;
    v->frame(click, *c, st);
    UiInput up;
    up.mx = click.mx;
    up.my = click.my;
    up.released = true;
    v->frame(up, *c, st);
    v->frame(none, *c, st);
    // the land's left edge is now in the middle of the map: its leftmost node sits right of centre
    u32 left = 0;
    for (u32 n = 1; n < f.node.size(); ++n)
        if (v->current().node[n].x < v->current().node[left].x)
            left = n;
    i32 lx = 0, ly = 0;
    v->node_screen(left, &lx, &ly);
    CHECK(lx > 300);
    // Double-click a node: its landmass fills the map (zoomed in), the node in view.
    UiInput reset;
    reset.mx = 700;
    reset.my = 500;
    reset.dbl = reset.pressed = reset.released = true;
    v->frame(reset, *c, st); // over water or land: either fits or resets
    v->frame(none, *c, st);
    u32 small = 0, small_nodes = 0xFFFFFFFFu;
    // the node on the smallest landmass with at least 3 nodes
    u32 count[512] = {};
    for (u32 n = 0; n < f.node.size(); ++n)
        if (v->current().node[n].mass < 512)
            ++count[v->current().node[n].mass];
    for (u32 n = 0; n < f.node.size(); ++n) {
        const u32 m = v->current().node[n].mass;
        if (m < 512 && count[m] >= 3 && count[m] < small_nodes) {
            small_nodes = count[m];
            small = n;
        }
    }
    i32 sx = 0, sy = 0;
    if (v->node_screen(small, &sx, &sy)) {
        const double z0 = v->zoom();
        UiInput dbl;
        dbl.mx = sx;
        dbl.my = sy;
        dbl.dbl = dbl.pressed = dbl.released = true;
        v->frame(dbl, *c, st);
        v->frame(none, *c, st);
        CHECK(v->zoom() > z0);
        CHECK(v->node_screen(small, &sx, &sy));
    }
    delete c;
    delete v;
}

} // namespace

void suite_view() {
    test_png();
    test_colours();
    test_chart_log();
    test_stats_header();
    test_phylo();
    test_frame_matches_world();
    test_watching_changes_nothing();
    test_live_knobs();
    test_numbered_snapshots();
    test_genebank_log();
    test_archive();
}

void suite_viewer() {
    test_viewer_input();
    test_viewer_auto_and_render();
    test_viewer_log_cap();
    test_viewer_landscape();
}
