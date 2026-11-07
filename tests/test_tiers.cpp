// Disk tiers (docs/world-plan.md W4): the archive, a slow, near-permanent
// second disk tier after the fast disk in the same ledger.
//   - the keys and their defaults; the archive off changes nothing
//   - ALLOC's archive selector claims only archive; RELEASE per tier
//   - a body's disk is its extents in claim order, addressed from 0 whatever
//     their tier: store, compare and load across mixed holdings
//   - costs: archive.slow times the disk's, the slowest tier for access, each
//     codon at its own tier's price; the substrate speeds divide them
//   - archive rot and fossil decay at a hundredth of the disk's
//   - DIVIDE, ENGULF, death (fossils with stamps), dispersal and dormancy
//     carry tiers; the dormant clock spends the fast disk's reserve first
//   - fail_disk_tier wipes one tier only
//   - a busy landscape with the archive on: the same at 1, 4 and 20 threads,
//     under shuffled dispatch and through a snapshot
//   - with substrates (the integration): each node's fast-disk and slow-disk
//     levels divide its tiers' DISK costs, and the busy landscape with
//     substrates and sparks by activity on too is as deterministic
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/landscape.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

// A quiet soup (1024 codons, 1000 millicores) with a fast disk and an
// archive, and a program planted at 100.
struct TierFix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;
    bool init(const char* prog, u32 disk, u32 archive, const char* extra = "", u32 units = 500) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.disk = ");
        cfg.append_u64(disk);
        cfg.append("\nworld.archive = ");
        cfg.append_u64(archive);
        cfg.append_char('\n');
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 3, 0);
        b = plant(s, 100, prog, units);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    // Runs the instruction at ip on the planted program's CPU.
    i64 op(u32 ip, u32 ax, u32 bx, u32 cx) {
        cpu().ip = ip;
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
        return s.run_cpu(c, 1);
    }
    // The same on another body's first CPU.
    i64 op_on(u32 body, u32 ip, u32 ax, u32 bx, u32 cx) {
        Cpu& k = s.cpu_mut(s.body(body).cpus);
        k.ip = ip;
        k.ax = ax;
        k.bx = bx;
        k.cx = cx;
        return s.run_cpu(s.body(body).cpus, 1);
    }
};

// Whether [off, off + n) of body b's disk is all on the archive (want true)
// or all on the fast disk.
bool all_on(const Soup& s, u32 b, u32 off, u32 n, bool archive) {
    Array<u32> addr;
    if (!s.disk().addresses(b, off, n, &addr))
        return false;
    for (usize i = 0; i < addr.size(); ++i)
        if ((addr[i] >= s.disk().fast) != archive)
            return false;
    return true;
}

bool near(double a, double b) { return a >= b * (1 - 1e-12) && a <= b * (1 + 1e-12); }

void kill_now(Soup& s, u32 b) {
    for (u32 k = s.body(b).cpus; k != kNone; k = s.cpu(k).next)
        s.cpu_mut(k).halted = 1;
    s.death_sweep();
}

// --- the keys -----------------------------------------------------------------

// With no fallback (a real run) the archive is on: a quarter of the disk,
// rotting and decaying at a hundredth of its rates, ten times slower. The
// test runner's legacy defaults have it off, so ALLOC knows its old
// resources and the shuffled control draws as before.
void test_keys() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    SoupParams p;
    const bool made = make_params(&p, "", 16384);
    CHECK(made);
    if (made) {
        CHECK(p.disk == 4096 && p.archive == 1024 && p.archive_slow == 10.0);
        CHECK(p.archive_rot == p.disk_rot / 100 && near(p.archive_rot, 1e-10));
        CHECK(p.archive_decay == p.disk_decay / 100 && near(p.archive_decay, 1e-8));
        CHECK(p.disk_speed == 1.0f && p.archive_speed == 1.0f && p.sel.res_n == 7);
    }
    SoupParams q;
    CHECK(make_params(&q, "world.disk = 400\n", 16384) && q.archive == 100); // follows world.disk
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    SoupParams l;
    CHECK(make_params(&l, "world.disk = 400\n", 1024) && l.archive == 0 && l.sel.res_n == 4);
    // The shuffled control: without an archive exactly the old maps; with
    // one, a permutation of all seven resources.
    SoupParams a, b2, c;
    CHECK(make_params(&a, "isa.selectors = shuffled\nworld.disk = 256\n", 1024));
    CHECK(make_params(&b2, "isa.selectors = shuffled\nworld.disk = 256\nworld.archive = 0\n", 1024));
    CHECK(make_params(&c, "isa.selectors = shuffled\nworld.disk = 256\nworld.archive = 64\n", 1024));
    CHECK(memcmp(&a.sel, &b2.sel, sizeof(SelectorMap)) == 0 && a.sel.res_n == 4);
    u32 seen = 0;
    for (u32 i = 0; i < c.sel.res_n; ++i)
        seen |= 1u << c.sel.res[i];
    CHECK(c.sel.res_n == 7 && seen == 127 && c.sel.res[0] != 0);
    // A bad value is refused.
    SoupParams bad;
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse("archive.slow = 0.5\n", &pe) && !soup_params_from_config(cfg, 1024, &bad));
}

// --- holding it -------------------------------------------------------------------

// ALLOC's archive selector (6) claims only archive, the disk selector (3) only
// fast disk; each returns its run's offset in the body's whole disk. SENSE 15
// reads the whole disk, SENSE 16 the free fast disk (no new reading). RELEASE
// gives up the newest codons of its own tier. Without an archive BX 6 fails.
void test_selectors() {
    TierFix f;
    CHECK(f.init("ALLOC SENSE RELEASE INC", 256, 64));
    CHECK(f.p.sel.res_n == 7 && f.s.free_disk() == 256 && f.s.free_archive() == 64);
    f.op(100, 99, 3, 20); // fast
    CHECK(f.cpu().err == 0 && f.cpu().ax == 0);
    f.op(100, 99, 6, 10); // archive
    CHECK(f.cpu().err == 0 && f.cpu().ax == 20);
    f.op(100, 99, 3, 5); // fast again
    CHECK(f.cpu().err == 0 && f.cpu().ax == 30);
    CHECK(all_on(f.s, f.b, 0, 20, false) && all_on(f.s, f.b, 20, 10, true) && all_on(f.s, f.b, 30, 5, false));
    CHECK(f.s.disk_held(f.b) == 35 && f.s.archive_held(f.b) == 10);
    CHECK(f.s.free_disk() == 231 && f.s.free_archive() == 54);
    f.op(100, 0, 6, 55); // more than the archive has free
    CHECK(f.cpu().err == 1 && f.s.archive_held(f.b) == 10);
    // Many claims of each kind land in their own tier only.
    bool tiers_ok = true;
    for (u32 i = 0; i < 12; ++i) {
        const u32 before = f.s.disk_held(f.b);
        f.op(100, 0, i & 1 ? 6u : 3u, 1 + i % 3);
        tiers_ok = tiers_ok && f.cpu().err == 0 && all_on(f.s, f.b, before, 1 + i % 3, (i & 1) != 0);
    }
    CHECK(tiers_ok);
    const u32 held = f.s.disk_held(f.b), arch = f.s.archive_held(f.b);
    f.op(101, 0, 15, 0);
    CHECK(f.cpu().err == 0 && f.cpu().cx == held);
    f.op(101, 0, 16, 0);
    CHECK(f.cpu().err == 0 && f.cpu().cx == f.s.free_disk() && f.cpu().cx == 256 - (held - arch));
    // RELEASE per tier: the newest archive codons only, then the newest fast.
    f.op(102, 0, 6, 4);
    CHECK(f.cpu().err == 0 && f.s.archive_held(f.b) == arch - 4 && f.s.disk_held(f.b) == held - 4);
    f.op(102, 0, 3, 3);
    CHECK(f.cpu().err == 0 && f.s.archive_held(f.b) == arch - 4 && f.s.disk_held(f.b) == held - 7);
    f.op(102, 0, 6, 1000); // all the archive
    CHECK(f.cpu().err == 0 && f.s.archive_held(f.b) == 0 && f.s.free_archive() == 64);
    f.op(102, 0, 6, 1); // none left: fails
    CHECK(f.cpu().err == 1);
    CHECK(soup_ok(f.s));
    // Off: BX 6 is past the map, and the fast disk is the whole ledger.
    TierFix g;
    CHECK(g.init("ALLOC INC", 256, 0));
    g.op(100, 0, 6, 8);
    CHECK(g.cpu().err == 1 && g.p.sel.res_n == 4 && !g.s.disk().tiered() && g.s.free_archive() == 0);
    // An archive alone (no fast disk): BX 3 fails, BX 6 works, DISK too.
    TierFix h;
    CHECK(h.init("ALLOC DISK INC INC", 0, 64));
    h.op(100, 0, 3, 8);
    CHECK(h.cpu().err == 1);
    h.op(100, 0, 6, 8);
    CHECK(h.cpu().err == 0 && h.s.archive_held(h.b) == 8 && h.p.sel.sense_n >= kSenseReadingsDisk);
    h.cpu().rh = 100;
    h.op(101, 0, 1, 4);
    CHECK(h.cpu().err == 0 && h.s.tally().archive_stores == 1 && soup_ok(h.s));
}

// A body's disk is all its extents in claim order, addressed from 0 whatever
// their tier: a store, a compare and a load across fast, archive and fast
// again move exactly what they should.
void test_mixed() {
    TierFix f;
    CHECK(f.init("DISK ALLOC INC INC INC INC INC INC", 256, 64));
    f.op(101, 0, 3, 4); // [0, 4) fast
    f.op(101, 0, 6, 4); // [4, 8) archive
    f.op(101, 0, 3, 4); // [8, 12) fast
    CHECK(all_on(f.s, f.b, 0, 4, false) && all_on(f.s, f.b, 4, 4, true) && all_on(f.s, f.b, 8, 4, false));
    u8 code[8];
    memcpy(code, f.s.mem() + 100, 8);
    f.cpu().rh = 102;
    f.op(100, 1, 1, 10); // store 10 codons from 102 at offset 1: fast, archive, fast
    CHECK(f.cpu().err == 0 && f.s.tally().disk_stores == 1 && f.s.tally().archive_stores == 1);
    Array<u32> addr;
    CHECK(f.s.disk().addresses(f.b, 1, 10, &addr));
    bool same = true;
    for (u32 i = 0; i < 10; ++i)
        same = same && f.s.disk().mem[addr[i]] == code[(2 + i) % 8];
    CHECK(same);
    // A compare across the tiers, with two codons damaged on the archive.
    f.s.disk_mut().mem[addr[4]] ^= 1;
    f.s.disk_mut().mem[addr[5]] ^= 2;
    f.cpu().rh = 102;
    f.op(100, 1, 0, 10);
    CHECK(f.cpu().err == 0 && f.cpu().cx == 2 && f.s.tally().disk_compares == 1);
    // A load across them into a block of mine.
    f.op(101, 0, 0, 16); // ALLOC 16 codons of memory
    const u32 k = f.s.body(f.b).latest;
    CHECK(k != kNone);
    const u32 at = f.s.block(k).start;
    f.cpu().wh = at;
    f.op(100, 0, 2, 12);
    CHECK(f.cpu().err == 0 && f.s.tally().disk_loads == 1 && f.s.tally().archive_loads == 1);
    Array<u32> all;
    CHECK(f.s.disk().addresses(f.b, 0, 12, &all));
    bool loaded = true;
    for (u32 i = 0; i < 12; ++i)
        loaded = loaded && f.s.mem()[at + i] == f.s.disk().mem[all[i]];
    CHECK(loaded);
    // A load from the fast disk alone doesn't count as the archive's.
    f.cpu().wh = at;
    f.op(100, 8, 2, 4);
    CHECK(f.cpu().err == 0 && f.s.tally().disk_loads == 2 && f.s.tally().archive_loads == 1);
    CHECK(soup_ok(f.s));
}

// --- costs -------------------------------------------------------------------

// classic: a DISK op costs disk.cost_base + disk.cost_per_codon x CX on the
// fast disk, archive.slow (10) times that on the archive, and across both the
// archive's access plus each codon at its own tier's price. The substrate
// speeds divide each tier's costs.
void test_costs() {
    TierFix f;
    CHECK(f.init("DISK ALLOC INC INC INC INC INC INC", 256, 64));
    f.op(101, 0, 3, 8); // [0, 8) fast
    f.op(101, 0, 6, 8); // [8, 16) archive
    f.cpu().rh = 100;
    CHECK(f.op(100, 0, 1, 8) == 4 + 2);                  // fast: 4 + 0.25 x 8
    CHECK(f.op(100, 8, 1, 8) == 40 + 20);                // archive: 10 x that
    CHECK(f.op(100, 4, 1, 8) == 40 + 1 + 10);            // 4 fast + 4 archive: the archive's access
    f.p.disk_speed = 2.0f;                               // a fast disk twice as quick
    CHECK(f.op(100, 0, 1, 8) == 3);
    f.p.disk_speed = 1.0f;
    f.p.archive_speed = 2.0f;                            // an archive twice as quick
    CHECK(f.op(100, 8, 1, 8) == 30);
    f.p.archive_speed = 1.0f;
    CHECK(soup_ok(f.s));

    // home_v1: an op longer than a tick is an action of as many ticks as it
    // takes -- a kernel call, the slowest tier's access, then the codons.
    TierFix t;
    CHECK(t.init("ALLOC DISK INC INC INC INC INC INC INC INC INC INC INC INC INC INC", 256, 64,
                 "cost.table = home_v1\ncpu.instr_per_core = 25000\n", 1000));
    const CostTable& ct = t.p.cost;
    auto ticks = [&](u64 ps) {
        const double d = static_cast<double>(ps) / t.p.tick_units;
        const u64 w = static_cast<u64>(d);
        return static_cast<double>(w) < d ? w + 1 : w;
    };
    t.op(100, 0, 3, 8); // [0, 8) fast
    t.op(100, 0, 6, 8); // [8, 16) archive
    struct Case {
        u32 off, bx;
        u64 want;
    };
    const Case cases[] = {
        {0, 1, ticks(ct.call + ct.disk_write + 8 * ct.disk_write_codon)},                       // fast store
        {8, 1, ticks(ct.call + 10 * ct.disk_write + 8 * 10 * ct.disk_write_codon)},             // archive store
        {8, 2, ticks(ct.call + 10 * ct.disk_read + 8 * 10 * ct.disk_read_codon)},               // archive load
        {4, 2, ticks(ct.call + 10 * ct.disk_read + 4 * ct.disk_read_codon + 40 * ct.disk_read_codon)}, // mixed
    };
    u64 tick = 0;
    bool ends = true;
    for (const Case& k : cases) {
        t.s.begin_tick(++tick);
        Cpu& c = t.cpu();
        c.ip = 101, c.ax = k.off, c.bx = k.bx, c.cx = 8, c.rh = 102, c.wh = 102;
        c.halted = 0;
        t.s.run_cpu(t.c, 100000000);
        const bool one = t.s.actions().size() == 1;
        ends = ends && one && t.s.actions()[0].end == tick + k.want;
        printf("  timed DISK (offset %u, op %u): %llu ticks\n", k.off, k.bx, static_cast<unsigned long long>(k.want));
        // Let it land (the store writes, the load fails into its own code: either way it ends).
        tick += k.want;
        t.s.begin_tick(tick);
        t.s.run_cpu(t.c, 0);
        ends = ends && t.s.actions().empty();
    }
    CHECK(ends);
    CHECK(cases[1].want >= 25 && cases[2].want >= 95 && cases[0].want <= 3); // a random archive read: about 100 ticks
    CHECK(soup_ok(t.s));
}

// --- hazards ------------------------------------------------------------------

u32 changed(const u8* a, const u8* b, u32 lo, u32 hi) {
    u32 n = 0;
    for (u32 i = lo; i < hi; ++i)
        n += a[i] != b[i];
    return n;
}

u32 bits(const Soup& s, u32 lo, u32 hi) {
    u32 n = 0;
    for (u32 a = lo; a < hi; ++a)
        for (u32 k = 0; k < 6; ++k)
            n += (s.disk().mem[a] >> k) & 1;
    return n;
}

// The archive rots at a hundredth of the disk's rate (archive.rot), and its
// fossils decay at a hundredth of the disk's (archive.decay); set outright,
// it rots as fast as asked.
void test_hazards() {
    for (int outright = 0; outright < 2; ++outright) {
        TierFix f;
        CHECK(f.init("INC", 256, 256, outright ? "disk.rot = 0.01\narchive.rot = 0.01\n" : "disk.rot = 0.01\n"));
        CHECK(near(f.p.archive_rot, outright ? 0.01 : 0.0001));
        Array<u8> before;
        before.assign(f.s.disk().mem.data(), f.s.disk().size());
        u64 flips = 0;
        for (u64 t = 0; t < 100; ++t) {
            f.s.tick(t);
            flips += f.s.tally().disk_rot;
        }
        const u32 fast = changed(before.data(), f.s.disk().mem.data(), 0, 256);
        const u32 arch = changed(before.data(), f.s.disk().mem.data(), 256, 512);
        printf("  rot over 100 ticks: fast disk %u codons changed, archive %u%s\n", fast, arch,
               outright ? " (archive.rot = disk.rot)" : "");
        CHECK(fast > 100 && fast < 220);
        CHECK(outright ? (arch > 100 && arch < 220) : arch < 12);
        CHECK(flips > 180 && flips < (outright ? 680u : 340u));
    }
    TierFix d;
    CHECK(d.init("INC", 256, 256, "disk.rot = 0\ndisk.decay = 0.05\n"));
    CHECK(near(d.p.archive_decay, 0.0005) && d.p.archive_rot == 0);
    const u32 fb = bits(d.s, 0, 256), ab = bits(d.s, 256, 512);
    for (u64 t = 0; t < 20; ++t)
        d.s.tick(t);
    const u32 fl = fb - bits(d.s, 0, 256), al = ab - bits(d.s, 256, 512);
    printf("  fossil decay over 20 ticks: fast disk %u bits lost, archive %u\n", fl, al);
    CHECK(fl > 120 && al < 12);
}

// --- carrying tiers -------------------------------------------------------------

// DIVIDE hands over the newest share of each tier, contents and all, in claim
// order; ENGULF takes all of it, each extent keeping its tier.
void test_divide_engulf() {
    TierFix f;
    CHECK(f.init("ALLOC DIVIDE INC INC", 256, 64));
    f.op(100, 0, 3, 32); // [0, 32) fast
    f.op(100, 0, 6, 32); // [32, 64) archive
    Array<u32> ft, at;
    CHECK(f.s.disk().addresses(f.b, 24, 8, &ft) && f.s.disk().addresses(f.b, 56, 8, &at));
    f.op(100, 0, 0, 8);  // ALLOC 8 codons of memory
    f.op(101, 0, 0, 16); // DIVIDE here, share 16/64
    CHECK(f.cpu().err == 0);
    const u32 d = f.s.body_at(f.s.body_count() - 1);
    CHECK(d != f.b && f.s.disk_held(d) == 16 && f.s.archive_held(d) == 8);
    CHECK(f.s.disk_held(f.b) == 48 && f.s.archive_held(f.b) == 24);
    Array<u32> got;
    CHECK(f.s.disk().addresses(d, 0, 16, &got));
    bool kept = true; // her fast share first, then her archive share, as the parent claimed them
    for (u32 i = 0; i < 8; ++i)
        kept = kept && got[i] == ft[i] && got[8 + i] == at[i];
    CHECK(kept && soup_ok(f.s));

    // ENGULF (a perfect match swallows whole): the prey's disk, both tiers.
    TierFix g;
    CHECK(g.init("ENGULF NOP0 NOP0 NOP1 INC", 256, 64));
    const u32 prey = plant(g.s, 200, "NOP1 NOP1 NOP0 ALLOC", 300);
    CHECK(prey != kNone);
    g.op_on(prey, 203, 0, 6, 8); // archive first
    g.op_on(prey, 203, 0, 3, 8); // then fast
    g.op(100, 0, 0, 0);
    CHECK(g.s.tally().engulfs == 1 && !g.s.body(prey).alive);
    CHECK(g.s.disk_held(g.b) == 16 && g.s.archive_held(g.b) == 8);
    CHECK(all_on(g.s, g.b, 0, 8, true) && all_on(g.s, g.b, 8, 8, false));
    g.s.death_sweep();
    CHECK(soup_ok(g.s));
}

// Death frees both tiers; their contents stay as fossils, stamped with the
// dead program's genome length, and loading archive fossils is salvage.
void test_death() {
    TierFix f;
    CHECK(f.init("DISK ALLOC INC INC INC INC INC INC", 256, 64, "salvage.ratio = 1\n"));
    f.op(101, 0, 3, 8);  // [0, 8) fast
    f.op(101, 0, 6, 64); // [8, 72) the whole archive
    for (u32 off = 0; off < 72; off += 8) {
        f.cpu().rh = 100;
        f.op(100, off, 1, 8); // my code everywhere
    }
    Array<u32> addr;
    CHECK(f.s.disk().addresses(f.b, 0, 72, &addr));
    kill_now(f.s, f.b);
    bool fossils = true;
    for (u32 i = 0; i < 72; ++i)
        fossils = fossils && f.s.disk().owner[addr[i]] == kNone && f.s.disk().stamp[addr[i]] == 8 &&
                  f.s.disk().mem[addr[i]] == f.s.mem()[100 + i % 8];
    CHECK(fossils && f.s.free_disk() == 256 && f.s.free_archive() == 64 && soup_ok(f.s));
    // A two-codon program claims archive (stamps kept: salvage) and loads it:
    // a stamp of 8 over 1 x 2 comes through intact a quarter of the time.
    const u32 small = plant(f.s, 300, "DISK ALLOC", 100);
    CHECK(small != kNone);
    f.op_on(small, 301, 0, 6, 32);
    CHECK(f.s.archive_held(small) == 32);
    f.op_on(small, 301, 0, 0, 32); // 32 codons of memory to load into
    const u32 k = f.s.body(small).latest;
    f.s.cpu_mut(f.s.body(small).cpus).wh = f.s.block(k).start;
    f.op_on(small, 300, 0, 2, 32);
    CHECK(f.s.tally().archive_loads == 1 && f.s.tally().garbled > 8 && f.s.tally().garbled < 32);
    CHECK(soup_ok(f.s));
}

// Dispersal: each tier's share is released at home (fossils) and travels as
// a count; on arrival she claims each in its own tier, capped by what's free.
void test_dispersal() {
    TierFix f;
    CHECK(f.init("ALLOC DIVIDE INC INC", 256, 64, "world.dispersal = true\n"));
    u32 edges[1] = {0};
    u32 in_flight[1] = {0};
    DispersalPorts ports;
    ports.edges = edges;
    ports.in_flight = in_flight;
    ports.degree = 1;
    ports.capacity = 2;
    f.s.set_ports(ports);
    f.op(100, 0, 3, 32);
    f.op(100, 0, 6, 16);
    f.op(100, 0, 0, 8);
    f.op(101, 0, 1, 32); // DIVIDE to neighbour 1, share 32/64
    CHECK(f.cpu().err == 0 && f.s.departures().size() == 1);
    const Departure& dep = f.s.departures()[0];
    CHECK(dep.disk == 16 && dep.archive == 8);
    CHECK(f.s.disk_held(f.b) == 24 && f.s.archive_held(f.b) == 8);
    CHECK(f.s.free_disk() == 240 && f.s.free_archive() == 56 && soup_ok(f.s));
    // She arrives on a node whose archive has only 5 codons free.
    TierFix g;
    CHECK(g.init("ALLOC INC", 256, 64));
    g.op(100, 0, 6, 59);
    CargoTag tag = {};
    CHECK(g.s.arrive(f.s.departure_codons() + dep.codon_off, dep.len, 10, 0, 0, dep.disk, tag, kFormCpu, 0, kKindP,
                     dep.archive));
    const u32 nb = g.s.body_at(g.s.body_count() - 1);
    CHECK(g.s.disk_held(nb) == 16 + 5 && g.s.archive_held(nb) == 5);
    CHECK(all_on(g.s, nb, 0, 16, false) && all_on(g.s, nb, 16, 5, true) && soup_ok(g.s));
}

// Dormancy (design.md 6.17): the disk reserve counts both tiers, each kept
// apart; the clock spends the memory reserve, then the fast disk's, then the
// archive's; waking claims each tier it held, empty, capped by what's free
// there.
void test_dormancy() {
    const char* extra = "life.dormancy = true\ndormant.disk_loss = 4\n";
    TierFix f;
    CHECK(f.init("ALLOC INC INC INC", 256, 64, extra));
    f.op(100, 0, 3, 8);
    f.op(100, 0, 6, 8);
    CHECK(f.s.force_dormant(f.b) && f.s.dormant(f.b));
    CHECK(f.s.dormant_state(f.b).disk_reserve == 8 && f.s.dormant_state(f.b).archive_reserve == 8);
    CHECK(f.s.disk_held(f.b) == 0 && f.s.free_disk() == 256 && f.s.free_archive() == 64 && soup_ok(f.s));
    const u32 hog = plant(f.s, 400, "INC INC INC INC", f.s.free_units());
    CHECK(hog != kNone && f.s.free_units() == 0);
    const u32 want[4][2] = {{4, 8}, {0, 8}, {0, 4}, {0, 0}}; // fast first
    bool clock = true;
    for (u32 t = 0; t < 3; ++t) {
        f.s.begin_tick(1 + t);
        f.s.death_sweep();
        clock = clock && f.s.dormant(f.b) && f.s.dormant_state(f.b).disk_reserve == want[t][0] &&
                f.s.dormant_state(f.b).archive_reserve == want[t][1];
    }
    CHECK(clock);
    f.s.begin_tick(4);
    f.s.death_sweep();
    CHECK(!f.s.body(f.b).alive && f.s.tally().dormant_expired == 1 && soup_ok(f.s));

    // Waking on its own freed cores: each tier again, the archive capped.
    TierFix g;
    CHECK(g.init("ALLOC INC INC INC", 256, 64, extra));
    g.op(100, 0, 3, 16);
    g.op(100, 0, 6, 8);
    const u32 other = plant(g.s, 400, "ALLOC INC", 10);
    CHECK(other != kNone);
    Rng r(1, 0, 0);
    CHECK(g.s.disk_mut().claim_any(r, other, 52, true) == 52); // the archive: 4 free
    CHECK(g.s.force_dormant(g.b) && g.s.free_archive() == 12);
    CHECK(g.s.disk_mut().claim_any(r, other, 8, true) == 8); // ... and 4 again
    CHECK(g.s.free_archive() == 4 && g.s.archive_held(other) == 60);
    g.s.begin_tick(1);
    g.s.death_sweep();
    CHECK(g.s.body(g.b).alive && !g.s.dormant(g.b) && g.s.tally().dormant_woke == 1);
    CHECK(g.s.disk_held(g.b) == 16 + 4 && g.s.archive_held(g.b) == 4 && soup_ok(g.s));

    // A bite of a resting dormant program takes its share of both reserves.
    TierFix h;
    CHECK(h.init("ENGULF INC INC INC", 256, 64, "life.dormancy = true\nengulf.bite = true\n"));
    CHECK(plant(h.s, 90, "NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1", 100) != kNone); // no waste behind
    const u32 prey = plant(h.s, 104, "NOP1 ALLOC INC INC INC INC INC INC", 100);
    CHECK(prey != kNone);
    h.op_on(prey, 105, 0, 3, 16);
    h.op_on(prey, 105, 0, 6, 16);
    CHECK(h.s.force_dormant(prey));
    h.op(100, 0, 0, 4); // a blind bite of 4 of its 8 codons
    CHECK(h.s.tally().bites_body == 1 && h.s.dormant(prey));
    CHECK(h.s.dormant_state(prey).disk_reserve == 8 && h.s.dormant_state(prey).archive_reserve == 8);
}

// fail_disk_tier (disasters X5, amended by W4): one tier's codons become
// INERT (stamps cleared), its holdings are released and resting programs lose
// that tier's reserve; the other tier is untouched.
void test_fail_tier() {
    TierFix f;
    CHECK(f.init("DISK ALLOC INC INC INC INC INC INC", 256, 64, "salvage.ratio = 4\nlife.dormancy = true\n"));
    f.op(101, 0, 3, 16);
    f.op(101, 0, 6, 8);
    for (u32 off = 0; off < 24; off += 8) {
        f.cpu().rh = 100;
        f.op(100, off, 1, 8);
    }
    const u32 other = plant(f.s, 300, "ALLOC INC", 100);
    f.op_on(other, 300, 0, 3, 8);
    f.op_on(other, 300, 0, 6, 8);
    kill_now(f.s, other); // fossils on both tiers, stamped
    const u32 sleeper = plant(f.s, 400, "ALLOC INC", 100);
    f.op_on(sleeper, 400, 0, 3, 8);
    f.op_on(sleeper, 400, 0, 6, 8);
    CHECK(f.s.force_dormant(sleeper));
    Array<u8> arch;
    arch.assign(f.s.disk().mem.data() + 256, 64);
    Array<u32> astamp;
    astamp.assign(f.s.disk().stamp.data() + 256, 64);
    CHECK(f.s.fail_disk_tier(false) == 256);
    bool inert = true;
    for (u32 a = 0; a < 256; ++a)
        inert = inert && f.s.disk().mem[a] == 0 && f.s.disk().stamp[a] == 0;
    CHECK(inert && f.s.free_disk() == 256 && f.s.archive_held(f.b) == 8 && f.s.disk_held(f.b) == 8);
    CHECK(memcmp(arch.data(), f.s.disk().mem.data() + 256, 64) == 0);
    CHECK(memcmp(astamp.data(), f.s.disk().stamp.data() + 256, 64 * sizeof(u32)) == 0);
    CHECK(f.s.dormant_state(sleeper).disk_reserve == 0 && f.s.dormant_state(sleeper).archive_reserve == 8);
    CHECK(f.s.body(f.b).alive && soup_ok(f.s));
    CHECK(f.s.fail_disk_tier(true) == 64);
    bool gone = true;
    for (u32 a = 256; a < 320; ++a)
        gone = gone && f.s.disk().mem[a] == 0 && f.s.disk().stamp[a] == 0;
    CHECK(gone && f.s.disk_held(f.b) == 0 && f.s.free_archive() == 64);
    CHECK(f.s.dormant_state(sleeper).archive_reserve == 0 && soup_ok(f.s));
    // Without an archive, the archive's failure is nothing; the disk's is all of it.
    TierFix g;
    CHECK(g.init("ALLOC INC", 256, 0));
    g.op(100, 0, 3, 8);
    CHECK(g.s.fail_disk_tier(true) == 0 && g.s.disk_held(g.b) == 8);
    CHECK(g.s.fail_disk_tier(false) == 256 && g.s.disk_held(g.b) == 0 && soup_ok(g.s));
}

// With disk.path (Phase 7b) the archive lives in the same files as the fast
// disk, after it: only the storage changes, so the soup is the same.
void test_files() {
    const char* dir = "nova_test_tiers_files";
    CHECK(make_folder(dir));
    {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.disk = 256\nworld.archive = 64\nsalvage.ratio = 4\n");
        SoupParams p;
        CHECK(make_params(&p, cfg.c_str(), 1024));
        Soup ram, file;
        file.set_disk_folder(dir);
        ram.init(&p, 3, 0);
        file.init(&p, 3, 0);
        CHECK(!ram.disk().mem.in_file() && file.disk().mem.in_file() && file.disk().stamp.in_file());
        CHECK(file.disk().size() == 320 && file.disk().fast == 256 && file.free_archive() == 64);
        Soup* both[2] = {&ram, &file};
        for (Soup* s : both) {
            const u32 b = plant(*s, 100, "DISK ALLOC INC INC INC INC INC INC", 500);
            Cpu& c = s->cpu_mut(s->body(b).cpus);
            c.ip = 101, c.bx = 6, c.cx = 16;
            s->run_cpu(s->body(b).cpus, 1);
            c.ip = 100, c.ax = 0, c.bx = 1, c.cx = 16, c.rh = 100;
            s->run_cpu(s->body(b).cpus, 1);
            for (u64 t = 0; t < 20; ++t)
                s->tick(t);
        }
        Hasher hr(1), hf(1);
        ram.hash_into(hr);
        file.hash_into(hf);
        CHECK_EQ_U64(hf.finish(), hr.finish());
        CHECK(soup_ok(file));
    }
    remove_folder(dir);
}

// --- worlds -------------------------------------------------------------------

// A busy landscape with the archive on: spread by its own distribution (some
// sites barren of it), dispersal, dormancy, the lifecycle (ruins with an
// archive), salvage and the shuffled control (ALLOC's map has the archive).
const char* const kTierWorld = "seed = 7\n"
                               "world.model = soup\n"
                               "world.graph = landscape\n"
                               "world.nodes = 48\n"
                               "land.scale = 4\n"
                               "land.resolution = 96\n"
                               "paths.hubs = 0.05\n"
                               "bridge.count = 2\n"
                               "bridge.max_length = 0.6\n"
                               "world.soup_size = 2048\n"
                               "world.millicores = 600\n"
                               "world.disk = 512\n"
                               "world.archive = 256\n"
                               "res.disk.spread = 0.6\n"
                               "res.archive.spread = 0.8\n"
                               "res.archive.smooth = 0.5\n"
                               "res.archive.barren = 0.2\n"
                               "disk.rot = 0.0001\n"
                               "salvage.ratio = 4\n"
                               "isa.selectors = shuffled\n"
                               "edge.bandwidth = 8\n"
                               "node.endurance = 4000\n"
                               "node.fallow_min = 20\n"
                               "node.recover_every = 10\n"
                               "life.frozen_ticks = 40\n"
                               "life.dormancy = true\n"
                               "mem.upkeep = 6\n"
                               "cpu.instr_per_core = 2500\n"
                               "cpu.wear = 0.002\n"
                               "spark.timing = last\n"
                               "spark.initial = 20\n"
                               "spark.rate = 0.5\n"
                               "spark.grant = 20\n"
                               "spark.min_len = 8\n"
                               "spark.max_len = 24\n"
                               "seed.genome = ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE\n"
                               "seed.at = 600\n"
                               "seed.millicores = 200\n"
                               "hazard.copy = 0.005\n"
                               "hazard.flaw = 0.001\n"
                               "hazard.rot = 0.00005\n";

void test_world() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    u64 held_max = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, kTierWorld, threads[v]));
        if (v == 0) { // the archive by its own distribution: exact totals, barren sites
            u64 total = 0;
            u32 barren = 0, distinct = 0;
            const u32 n = run.world().node_count();
            for (u32 i = 0; i < n; ++i) {
                const u32 a = run.world().node_params(i).archive;
                total += a;
                barren += a == 0;
                distinct += i > 0 && a != run.world().node_params(i - 1).archive;
            }
            CHECK_EQ_U64(total, 256ull * n);
            CHECK(barren > 0 && distinct > n / 2);
        }
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            Rng r(99, 0, 0);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 300; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = run.world().last_soup_tick();
                sum.archive_stores += s.archive_stores;
                sum.archive_loads += s.archive_loads;
                sum.disk_stores += s.disk_stores;
                sum.dormant_started += s.dormant_started;
                sum.dormant_woke += s.dormant_woke;
                sum.collapses += s.collapses;
                sum.recoveries += s.recoveries;
                u64 held = 0;
                for (u32 n = 0; n < run.world().node_count(); ++n)
                    held += run.world().node_params(n).archive - run.world().node_soup(n).free_archive();
                held_max = held > held_max ? held : held_max;
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    printf("  archive world: 300 ticks, %llu archive stores and %llu loads (of %llu disk stores), archive held up to "
           "%llu, %llu went dormant, %llu woke, %llu collapses, %llu recoveries, hash %016llx\n",
           static_cast<unsigned long long>(sum.archive_stores), static_cast<unsigned long long>(sum.archive_loads),
           static_cast<unsigned long long>(sum.disk_stores), static_cast<unsigned long long>(held_max),
           static_cast<unsigned long long>(sum.dormant_started), static_cast<unsigned long long>(sum.dormant_woke),
           static_cast<unsigned long long>(sum.collapses), static_cast<unsigned long long>(sum.recoveries),
           static_cast<unsigned long long>(hashes[0]));
    CHECK(sum.archive_stores + sum.archive_loads > 0 && held_max > 0 && sum.dormant_started > 0);
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);

    // Through a snapshot taken midway.
    Run a;
    CHECK(make_run(a, kTierWorld, 4));
    a.run_until(150);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(300);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);

    // Off is unchanged: world.archive = 0 outright is the legacy world, and
    // turning the archive on changes it.
    TextBuf off, on;
    off.append(kSoupWorldProc);
    off.append(kPhase4All);
    on.append(off.view());
    off.append("world.archive = 0\n");
    on.append("world.archive = 256\n");
    u64 h[3] = {};
    const char* cfgs[3] = {nullptr, off.c_str(), on.c_str()};
    TextBuf legacy;
    legacy.append(kSoupWorldProc);
    legacy.append(kPhase4All);
    cfgs[0] = legacy.c_str();
    for (u32 i = 0; i < 3; ++i) {
        Run r;
        CHECK(make_run(r, cfgs[i], 4));
        r.run_until(100);
        h[i] = r.world().hash();
    }
    CHECK_EQ_U64(h[1], h[0]);
    CHECK(h[2] != h[0]);
}

// A snapshot from before the disk tiers (format v3, its config without
// world.archive) loads with the archive off, as it ran, and resumes exactly;
// on the engine's defaults the archive would otherwise be on and the disk
// section the wrong shape. (A v3 build's world also had the old spark rule,
// sparks by activity coming with the same v4: a ring has no substrates.)
void test_old_snapshot() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    const char* w = "world.model = soup\nworld.nodes = 4\nworld.soup_size = 4096\nspark.initial = 40\n"
                    "spark.rate = 0.25\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = cpu\n";
    TextBuf with;
    with.append(w);
    with.append("world.archive = 0\nspark.activity = 0\n");
    Run a;
    CHECK(make_run(a, with.c_str(), 2));
    a.run_until(30);
    Config old; // the config as a v3 build wrote it: no world.archive (nor spark.activity)
    Config::ParseError pe;
    CHECK(old.parse(w, &pe));
    ByteWriter snap;
    snapshot_write(a.world(), old, snap);
    a.run_until(60);
    Array<u8> bytes;
    bytes.assign(snap.data(), snap.size());
    bytes[8] = 3; // format v3
    const u64 sum = hash_bytes(bytes.data(), bytes.size() - 8, 0x6E6F76612D736E70ull); // "nova-snp"
    for (u32 i = 0; i < 8; ++i)
        bytes[bytes.size() - 8 + i] = static_cast<u8>(sum >> (8 * i));
    Run b;
    TextBuf err;
    const bool loaded = b.init_from_snapshot(bytes.data(), bytes.size(), 2, &err);
    CHECK(loaded);
    if (!loaded)
        fprintf(stderr, "  %s\n", err.c_str());
    if (loaded) {
        CHECK(b.world().node_params(0).archive == 0 && b.config().get_str("world.archive", "").eq("0"));
        b.run_until(60);
        CHECK_EQ_U64(b.world().hash(), a.world().hash());
    }
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

// --- with substrates (docs/world-plan.md W1 with W4; docs/results/integration.md) ---

// The classic costs of an 8-codon DISK store on each tier, run on a quiet
// soup with a node's own params: 8 fast codons claimed, then 8 of archive.
bool store_costs(const SoupParams& node, i64* fast, i64* arch) {
    SoupParams p = node; // (the soup keeps a pointer to its params)
    Soup s;
    s.init(&p, 3, 0);
    const u32 b = plant(s, 100, "DISK ALLOC INC INC INC INC INC INC", 500);
    if (b == kNone)
        return false;
    const u32 c = s.body(b).cpus;
    s.begin_tick(0);
    auto op = [&](u32 ip, u32 ax, u32 bx, u32 cx) {
        Cpu& k = s.cpu_mut(c);
        k.ip = ip;
        k.ax = ax;
        k.bx = bx;
        k.cx = cx;
        return s.run_cpu(c, 1);
    };
    op(101, 0, 3, 8); // [0, 8) fast
    op(101, 0, 6, 8); // [8, 16) archive
    if (s.disk_held(b) != 16 || s.archive_held(b) != 8)
        return false;
    s.cpu_mut(c).rh = 100;
    *fast = op(100, 0, 1, 8);
    const bool ok = s.cpu(c).err == 0;
    *arch = op(100, 8, 1, 8);
    return ok && s.cpu(c).err == 0 && soup_ok(s);
}

// Both ends connected: a world with substrates sets each node's disk_speed
// and archive_speed from its fast-disk and slow-disk levels (derive_node),
// and DISK divides each tier's costs by them. On every node of a procedural
// world, an 8-codon store costs 6 on the fast disk at 1x (4 + 0.25 x 8) and
// 60 on the archive: at 2x half that, at the floor (0.1) ten times.
void test_substrate_speeds() {
    const char* w = "seed = 3\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 40\n"
                    "world.soup_size = 1024\nworld.millicores = 1000\nworld.disk = 256\nworld.archive = 64\n"
                    "substrate.enabled = true\n";
    Run run;
    CHECK(make_run(run, w, 1));
    const World& wd = run.world();
    CHECK(wd.substrates());
    const i64 fast_want[kSubstrateLevels] = {60, 12, 6, 4, 3}; // the floor, 0.5x, 1x, 1.5x, 2x
    const i64 arch_want[kSubstrateLevels] = {600, 120, 60, 40, 30};
    u32 seen_f[kSubstrateLevels] = {}, seen_a[kSubstrateLevels] = {};
    bool speeds = true, costs = true;
    for (u32 n = 0; n < wd.node_count(); ++n) {
        const SoupParams& p = wd.node_params(n);
        const u8 lf = wd.substrate_level(n, kSubDiskFast), la = wd.substrate_level(n, kSubDiskSlow);
        speeds = speeds && p.disk_speed == wd.substrate_mult(n, kSubDiskFast) &&
                 p.archive_speed == wd.substrate_mult(n, kSubDiskSlow) && p.disk == 256 && p.archive == 64;
        i64 f = 0, a = 0;
        const bool ran = store_costs(p, &f, &a);
        costs = costs && ran && f == fast_want[lf] && a == arch_want[la];
        if (!ran || f != fast_want[lf] || a != arch_want[la])
            fprintf(stderr,
                    "  node %u: fast level %u costs %lld (want %lld), archive level %u costs %lld (want %lld)\n", n,
                    lf, static_cast<long long>(f), static_cast<long long>(fast_want[lf]), la, static_cast<long long>(a),
                    static_cast<long long>(arch_want[la]));
        ++seen_f[lf];
        ++seen_a[la];
    }
    CHECK(speeds && costs);
    bool every = true; // every level is on some node (40 nodes: 4, 8, 16, 8, 4 of each type)
    for (u32 l = 0; l < kSubstrateLevels; ++l)
        every = every && seen_f[l] == (l == 2 ? 16u : (l == 0 || l == 4 ? 4u : 8u)) && seen_a[l] == seen_f[l];
    CHECK(every);
    printf("  substrates and disk tiers: an 8-codon store costs 3 / 6 / 60 on a fast disk at 2x / 1x / the floor, "
           "30 / 60 / 600 on an archive, on all %u nodes\n",
           wd.node_count());
}

// The busy archive landscape with substrates and sparks by activity on too:
// every new feature of the varied world at once, the same at 1, 4 and 20
// threads, under shuffled dispatch and through a snapshot.
void test_varied_world() {
    TextBuf cfg;
    cfg.append(kTierWorld);
    cfg.append("substrate.enabled = true\nspark.activity = 2\n");
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    u32 slow_disks = 0;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        const World& w = run.world();
        if (v == 0)
            for (u32 n = 0; n < w.node_count(); ++n)
                slow_disks += w.node_params(n).disk_speed != 1.0 || w.node_params(n).archive_speed != 1.0;
        if (v == 3) {
            const u32 n = w.node_count();
            order.resize(n);
            Rng r(99, 0, 0);
            for (u32 i = 0; i < n; ++i)
                order[i] = i;
            for (u32 i = n - 1; i > 0; --i) {
                const u32 j = static_cast<u32>(r.below(i + 1));
                const u32 t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 300; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = w.last_soup_tick();
                sum.archive_stores += s.archive_stores;
                sum.archive_loads += s.archive_loads;
                sum.disk_stores += s.disk_stores;
                sum.disk_loads += s.disk_loads;
                sum.sparks += s.sparks;
            }
        }
        hashes[v] = w.hash();
        if (v == 0) {
            CHECK(w.substrates() && w.node_params(0).spark_activity == 2.0);
            for (u32 n = 0; n < w.node_count(); ++n)
                CHECK(soup_ok(w.node_soup(n)));
        }
    }
    printf("  varied world (substrates, sparks by activity, archive): 300 ticks, %llu sparks, %llu DISK stores "
           "(%llu on the archive), %llu loads (%llu), %u of %u nodes with a disk tier off 1x, hash %016llx\n",
           static_cast<unsigned long long>(sum.sparks), static_cast<unsigned long long>(sum.disk_stores),
           static_cast<unsigned long long>(sum.archive_stores), static_cast<unsigned long long>(sum.disk_loads),
           static_cast<unsigned long long>(sum.archive_loads), slow_disks, 48u,
           static_cast<unsigned long long>(hashes[0]));
    CHECK(sum.disk_stores > 0 && sum.archive_stores + sum.archive_loads > 0 && slow_disks > 0);
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    Run a; // through a snapshot taken midway, resumed on 20 threads
    CHECK(make_run(a, cfg.c_str(), 1));
    a.run_until(150);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 20, &err));
    b.run_until(300);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
}

} // namespace

void suite_tiers() {
    test_keys();
    test_selectors();
    test_mixed();
    test_costs();
    test_hazards();
    test_divide_engulf();
    test_death();
    test_dispersal();
    test_dormancy();
    test_fail_tier();
    test_files();
    test_world();
    test_old_snapshot();
    test_substrate_speeds();
    test_varied_world();
}
