// Phase 4 (design.md 6.10): the individual features, one suite each.
//   leak       the ambient heat leak
//   edges      transit corruption and link failures
//   disk       the disk tier, DISK, volatility and repair
//   signals    EMIT, LISTEN and propagation
//   lifecycle  wear, starvation, collapse, recovery and graph rebuilds
#include <cmath>
#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// kQuiet with heat on.
const char* const kWarm = "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n";

// A busy loop: runs its whole quota every tick.
const char* const kBusy = "INC INC INC INC INC INC INC INC";

bool near(double a, double b, double eps = 1e-4) { return fabs(a - b) <= eps; }

// A one-shot disperser: ALLOC 8, DIVIDE to neighbour 1 with share 32/64, then
// eight failing DIVIDEs halt it.
const char* const kOneShot = "seed.genome = ZERO INC SHL SHL SHL ALLOC ZERO INC ROTB ZERO INC SHL SHL SHL "
                             "SHL SHL DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE\n";

// Two nodes in a ring with dispersal, the one-shot disperser on node 0, an
// 8-codon cargo crossing in 3 ticks (bandwidth 4, latency 1).
void two_nodes(TextBuf& cfg, u64 seed, const char* extra) {
    cfg.append("seed = ");
    cfg.append_u64(seed);
    cfg.append("\nworld.model = soup\nworld.nodes = 2\nworld.soup_size = 512\n"
               "world.millicores = 1000\nworld.dispersal = true\nedge.bandwidth = 4\n"
               "seed.node = 0\nseed.at = 100\nseed.millicores = 1000\nheat.enabled = false\n"
               "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n");
    cfg.append(kOneShot);
    cfg.append(extra);
}

// A dispersing replicator (copies itself, DIVIDEs to its nearest neighbour).
const char* const kDisperser = "ZERO ROTB SENSE ALLOC NOP1 COPY DJNZ NOP0 ZERO INC ROTB SENSE DIVIDE";

} // namespace

// The ambient leak: heat.ambient x the mean CPU heat input over every
// millicore reaches every unit and page, held or free; off, idle units stay
// cold.
void suite_leak() {
    for (int on = 0; on < 2; ++on) {
        TextBuf cfg;
        cfg.append(kWarm);
        cfg.append("world.millicores = 1000\nworld.gpus = 10\n");
        cfg.append(on ? "heat.ambient = 0.5\n" : "");
        SoupParams p;
        CHECK(make_params(&p, cfg.c_str(), 1024));
        Soup s;
        s.init(&p, 1, 0);
        const u32 b = plant(s, 100, kBusy, 500);
        CHECK(b != kNone);
        s.tick(0);
        // Flat out: input 6 per held unit; the mean over 1000 units is 3.
        u32 free_unit = kNone, held_unit = kNone;
        for (u32 u = 0; u < p.millicores; ++u) {
            if (s.unit_owner(u) == kNone && free_unit == kNone)
                free_unit = u;
            if (s.unit_owner(u) == b && held_unit == kNone)
                held_unit = u;
        }
        CHECK(free_unit != kNone && held_unit != kNone);
        const double amb = on ? 1.5 : 0.0;
        CHECK(near(s.ambient(), amb));
        CHECK(near(s.unit_heat(free_unit), amb));
        CHECK(near(s.unit_heat(held_unit), 6.0 + amb));
        CHECK(near(s.page_heat(15), amb)); // a page nobody touched
        CHECK(near(s.gpu_heat(0), amb));   // a free GPU unit
    }
    // Off means unchanged: the default world hashes as before.
    Run run;
    CHECK(make_run(run, kSoupWorld1b, 4));
    run.run_until(50);
    TextBuf cfg;
    cfg.append(kSoupWorld1b);
    cfg.append("heat.ambient = 0\n");
    Run off;
    CHECK(make_run(off, cfg.c_str(), 4));
    off.run_until(50);
    CHECK_EQ_U64(run.world().hash(), off.world().hash());
    TextBuf hot;
    hot.append(kSoupWorld1b);
    hot.append("heat.ambient = 0.2\n");
    Run warm;
    CHECK(make_run(warm, hot.c_str(), 4));
    warm.run_until(50);
    CHECK(warm.world().hash() != run.world().hash());
}

namespace {

// A quiet soup (1024 codons, 1000 millicores) with a disk, and a program
// planted at 100 with 500 millicores.
struct DiskFix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;
    bool init(const char* prog, u32 disk, const char* extra = "") {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\nworld.disk = ");
        cfg.append_u64(disk);
        cfg.append_char('\n');
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 3, 0);
        b = plant(s, 100, prog, 500);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    i64 op(u32 ip, u32 ax, u32 bx, u32 cx) {
        cpu().ip = ip;
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
        return s.run_cpu(c, 1);
    }
};

} // namespace

// The disk tier and volatility.
void suite_disk() {
    // Off: DISK, ALLOC/RELEASE disk and SENSE 15-16 fail, as before.
    {
        DiskFix f;
        CHECK(f.init("DISK ALLOC SENSE", 0));
        f.op(100, 0, 1, 4);
        CHECK(f.cpu().err == 1);
        f.op(101, 0, 3, 8);
        CHECK(f.cpu().err == 1);
        CHECK(f.p.sel.sense_n == 8);
        f.op(102, 0, 15, 0);
        CHECK(f.cpu().err == 1 && f.s.free_disk() == 0);
    }
    // The ledger: ALLOC disk claims runs and returns their offsets; SENSE sees
    // holdings and the free disk; RELEASE gives up the newest codons.
    {
        DiskFix f;
        CHECK(f.init("ALLOC SENSE RELEASE", 256));
        CHECK(f.p.sel.sense_n == 17 && f.s.free_disk() == 256);
        f.op(100, 99, 3, 40);
        CHECK(f.cpu().err == 0 && f.cpu().ax == 0 && f.s.disk_held(f.b) == 40);
        f.op(100, 99, 3, 20);
        CHECK(f.cpu().err == 0 && f.cpu().ax == 40 && f.s.disk_held(f.b) == 60);
        f.op(101, 0, 15, 0);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 60);
        f.op(101, 0, 16, 0);
        CHECK(f.cpu().err == 0 && f.cpu().cx == 196);
        f.op(100, 0, 3, 500);
        CHECK(f.cpu().err == 1); // no run that long
        f.op(102, 0, 3, 30);
        CHECK(f.cpu().err == 0 && f.s.disk_held(f.b) == 30 && f.s.free_disk() == 226);
        CHECK(soup_ok(f.s));
    }
    // DISK: store, compare, load; all-or-nothing; costs.
    {
        DiskFix f;
        CHECK(f.init("DISK ALLOC INC INC INC INC INC INC", 256));
        f.op(101, 0, 3, 16); // 16 codons of disk
        const u8* m = f.s.mem();
        u8 before[8];
        for (u32 i = 0; i < 8; ++i)
            before[i] = m[100 + i];
        f.cpu().rh = 100;
        const i64 cost = f.op(100, 0, 1, 8); // store my 8 codons at offset 0
        CHECK(f.cpu().err == 0 && cost == 4 + 2 && f.cpu().rh == 100); // wrapped round my block
        u32 a0 = kNone;
        {
            Array<u32> addr;
            CHECK(f.s.disk().addresses(f.b, 0, 8, &addr));
            a0 = addr[0];
            bool same = true;
            for (u32 i = 0; i < 8; ++i)
                same = same && f.s.disk().mem[addr[i]] == before[i];
            CHECK(same);
        }
        f.s.mem_mut()[103] = 0;
        f.s.mem_mut()[105] = 0; // damage two codons
        f.cpu().rh = 100;
        f.op(100, 0, 0, 8); // compare
        CHECK(f.cpu().err == 0 && f.cpu().cx == 2);
        f.cpu().wh = 100;
        f.op(100, 0, 2, 8); // load: repaired
        CHECK(f.cpu().err == 0 && memcmp(f.s.mem() + 100, before, 8) == 0 && f.s.tally().disk_loads == 1);
        f.cpu().rh = 100;
        f.op(100, 10, 1, 8); // offset 10 + 8 > 16: fails, nothing moves
        CHECK(f.cpu().err == 1 && f.s.tally().disk_stores == 1);
        f.cpu().wh = 900; // waste: not writable
        f.op(100, 0, 2, 4);
        CHECK(f.cpu().err == 1);
        f.op(100, 0, 1, 0); // CX = 0 fails
        CHECK(f.cpu().err == 1);
        (void)a0;
        CHECK(soup_ok(f.s));
    }
    // Shares: DIVIDE hands over the newest share, contents and all; death
    // frees the disk and leaves its contents (fossils).
    {
        DiskFix f;
        CHECK(f.init("ALLOC DIVIDE", 256));
        f.op(100, 0, 3, 32);
        f.op(100, 0, 3, 32);
        Array<u32> addr;
        CHECK(f.s.disk().addresses(f.b, 48, 16, &addr));
        const u32 tail_start = addr[0];
        f.op(100, 0, 0, 8);  // ALLOC 8 codons of memory
        f.op(101, 0, 0, 16); // DIVIDE, share 16/64: a quarter of 64 disk codons
        const u32 d = f.s.body_at(f.s.body_count() - 1);
        CHECK(f.s.disk_held(f.b) == 48 && f.s.disk_held(d) == 16);
        CHECK(f.s.disk().addresses(d, 0, 16, &addr) && addr[0] == tail_start);
        CHECK(soup_ok(f.s));
        u8 fossil[16];
        for (u32 i = 0; i < 16; ++i)
            fossil[i] = f.s.disk().mem[addr[i]];
        // The daughter dies: her disk is freed, contents kept.
        f.s.cpu_mut(f.s.body(d).cpus).halted = 1;
        f.s.death_sweep();
        CHECK(f.s.disk_held(f.b) == 48 && f.s.free_disk() == 256 - 48);
        bool kept = true;
        for (u32 i = 0; i < 16; ++i)
            kept = kept && f.s.disk().mem[addr[i]] == fossil[i] && f.s.disk().owner[addr[i]] == kNone;
        CHECK(kept && soup_ok(f.s));
        // Moving all of one body's disk to another (ENGULF's path).
        const u32 b2 = plant(f.s, 600, "INC", 10);
        f.s.disk_mut().move_all(f.b, b2);
        CHECK(f.s.disk_held(f.b) == 0 && f.s.disk_held(b2) == 48 && soup_ok(f.s));
    }
    // Dispersal: the share is released at home and claimed on arrival.
    {
        TextBuf cfg;
        two_nodes(cfg, 1, "world.disk = 512\n");
        // The one-shot disperser holds no disk; give its arrival some through
        // a seeded genome that claims disk first.
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        const u8 code[4] = {8, 8, 8, 8};
        CargoTag tag = {};
        Soup& s1 = run.world().node_soup(1);
        s1.begin_tick(0);
        CHECK(s1.arrive(code, 4, 10, 0, 0, 100, tag));
        const u32 nb = s1.body_at(s1.body_count() - 1);
        CHECK(s1.disk_held(nb) == 100 && s1.free_disk() == 412 && soup_ok(s1));
    }
    // Disk rot.
    {
        DiskFix f;
        CHECK(f.init("INC", 256, "disk.rot = 0.01\n"));
        u64 flips = 0;
        for (u64 t = 0; t < 100; ++t) {
            f.s.tick(t);
            flips += f.s.tally().disk_rot;
        }
        CHECK(flips > 180 && flips < 340); // expected 256
    }
    // Volatility: wipes of whole pages, counted; a body wiped whole dies.
    {
        DiskFix f;
        CHECK(f.init(kBusy, 0, "mem.volatility = 0.01\nmem.wipe_pages = 2\n"));
        u64 wipes = 0, wiped = 0;
        for (u64 t = 0; t < 400; ++t) {
            f.s.tick(t);
            wipes += f.s.tally().wipes;
            wiped += f.s.tally().wiped;
        }
        CHECK(wipes > 40 && wipes < 100); // 16 pages x 0.01 x 400 = 64
        CHECK(wiped >= wipes * 64 && wiped <= wipes * 128 && soup_ok(f.s));
        // The busy body sits inside one page, so any wipe of that page is a
        // whole-body hit; nothing else can kill it in a quiet soup.
        DiskFix g;
        CHECK(g.init(kBusy, 0, "mem.volatility = 1\nmem.wipe_pages = 1\n"));
        u8 before[8];
        memcpy(before, g.s.mem() + 100, 8);
        u64 deaths = 0;
        for (u64 t = 0; t < 50 && g.s.body_count() > 0; ++t) {
            g.s.tick(t);
            deaths += g.s.tally().wipe_deaths;
        }
        CHECK(deaths == 1 && g.s.body_count() == 0 && memcmp(before, g.s.mem() + 100, 8) != 0);
    }
    // disk_user: a member of a gated lineage loading into its own block.
    {
        TextBuf cfg;
        cfg.append("seed = 9\nworld.model = soup\nworld.soup_size = 16384\nworld.millicores = 4000\n"
                   "cpu.instr_per_core = 25000\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                   "seed.at = 8000\nworld.nodes = 1\nworld.disk = 1024\n"
                   "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE\nseed.millicores = 10\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        run.run_until(20);
        CHECK(run.detection().found);
        Soup& s = run.world().node_soup(0);
        const Lineages* L = run.world().node_lineages(0);
        u32 member = kNone;
        for (u32 k = 0; k < s.block_slots() && member == kNone; ++k) {
            const BlockTag t = L->tag(k);
            if (t.lineage && s.block(k).body != kNone && L->lineage(t.lineage) && L->lineage(t.lineage)->detected)
                member = k;
        }
        CHECK(member != kNone);
        if (member != kNone) {
            const u32 body = s.block(member).body;
            Rng r(1, 0, 0);
            CHECK(s.disk_mut().alloc_run(r, body, 8) != kNone);
            const u32 at = s.block(member).start;
            u8 disk_codon = 0;
            for (u32 i = 0; i < 64; ++i)
                if (s.params().table.op[i] == Op::disk)
                    disk_codon = static_cast<u8>(i);
            s.mem_mut()[at] = disk_codon;
            const u32 cpu = s.body(body).cpus;
            Cpu& c = s.cpu_mut(cpu);
            c.ip = at, c.ax = 0, c.bx = 2, c.cx = 4, c.wh = at + 1, c.halted = 0;
            s.run_cpu(cpu, 1);
            CHECK(c.err == 0 && L->disk_detections() == 1);
            CHECK(!L->gates().empty() && L->gates()[L->gates().size() - 1].kind == kEventDiskUser);
        }
    }
}

namespace {

// n codons of INC after a prefix, as op names.
void with_incs(TextBuf& out, const char* prefix, u32 n) {
    out.append(prefix);
    for (u32 i = 0; i < n; ++i)
        out.append(" INC");
}

// Kills body b at once (its CPUs halt; the sweep takes it).
void kill_now(Soup& s, u32 b) {
    for (u32 c = s.body(b).cpus; c != kNone; c = s.cpu(c).next)
        s.cpu_mut(c).halted = 1;
    s.death_sweep();
}

} // namespace

// Remnants and salvage: stamps, the graded rule, engulfed prey, disk fossils.
void suite_salvage() {
    const char* const kRule = "salvage.ratio = 4\n";
    // Stamps: set at death to the genome length, cleared by a claim.
    {
        DiskFix f;
        CHECK(f.init("READ INC INC INC INC INC INC INC", 0, kRule)); // genome 8: free up to 32
        TextBuf big;
        with_incs(big, "INC", 127); // genome 128
        const u32 a = plant(f.s, 300, big.c_str(), 10);
        CHECK(a != kNone && f.s.stamp(300) == 0);
        kill_now(f.s, a);
        CHECK(f.s.stamp(300) == 128 && f.s.stamp(427) == 128 && f.s.stamp(428) == 0);
        // The rule: each codon comes through intact with chance 32/128.
        u32 intact = 0, reads = 0;
        for (u32 pass = 0; pass < 10; ++pass) {
            f.cpu().rh = 300;
            for (u32 i = 0; i < 128; ++i) {
                const u8 truth = f.s.mem()[f.cpu().rh];
                f.op(100, 0, 0, 0);
                intact += f.cpu().cx == truth;
                ++reads;
            }
        }
        const u64 garbled = f.s.tally().garbled;
        CHECK(intact > 270 && intact < 400); // expected 0.25 + 0.75 / 64 of 1280: ~335
        CHECK(garbled > 860 && garbled < 1050); // expected 960
        printf("  salvage: genome 8 reading a genome-128 corpse at K = 4: %u of %u codons intact (expected ~335)\n",
               intact, reads);
        // Within K x my genome: no limit.
        TextBuf small;
        with_incs(small, "INC", 23); // genome 24 <= 32
        const u32 b = plant(f.s, 600, small.c_str(), 10);
        kill_now(f.s, b);
        const u64 before = f.s.tally().garbled;
        f.cpu().rh = 600;
        bool all = true;
        for (u32 i = 0; i < 24; ++i) {
            const u8 truth = f.s.mem()[f.cpu().rh];
            f.op(100, 0, 0, 0);
            all = all && f.cpu().cx == truth;
        }
        CHECK(all && f.s.tally().garbled == before);
        // A claim over a remnant makes it wholly the claimer's.
        CHECK(f.s.create_body(300, 16, 0) != kNone && f.s.stamp(300) == 0 && f.s.stamp(320) == 128);
        CHECK(soup_ok(f.s));
    }
    // Engulfed prey: stamped with its genome length at the engulf, so a small
    // eater gets only pieces of a big prey's code.
    {
        DiskFix f;
        CHECK(f.init("INC", 0, kRule));
        const u32 eater = plant(f.s, 500, "ENGULF NOP0 NOP0 NOP1 READ", 400); // genome 5: free up to 20
        TextBuf prey_code;
        with_incs(prey_code, "NOP1 NOP1 NOP0", 97); // genome 100
        const u32 prey = plant(f.s, 600, prey_code.c_str(), 50);
        CHECK(eater != kNone && prey != kNone && f.s.stamp(650) == 0);
        const u32 ec = f.s.body(eater).cpus;
        f.s.run_cpu(ec, 1);
        CHECK(f.s.tally().engulfs == 1 && f.s.stamp(600) == 100 && f.s.stamp(699) == 100 && f.s.stamp(504) == 0);
        Cpu& c = f.s.cpu_mut(ec);
        c.rh = 600;
        u32 intact = 0;
        for (u32 i = 0; i < 1000; ++i) {
            const u8 truth = f.s.mem()[c.rh];
            c.ip = 504;
            c.errs = 0;
            f.s.run_cpu(ec, 1);
            intact += c.cx == truth;
        }
        CHECK(intact > 160 && intact < 270); // 0.2 + 0.8 / 64: ~212
    }
    // Disk fossils: a dead program's disk keeps its stamps when claimed, and
    // loading it is salvage; the loaded codons are the loader's own.
    {
        DiskFix f;
        CHECK(f.init("ALLOC DISK INC INC INC INC INC INC", 64, kRule)); // genome 8
        TextBuf big;
        with_incs(big, "ALLOC", 127); // genome 128
        const u32 a = plant(f.s, 300, big.c_str(), 10);
        Cpu& ac = f.s.cpu_mut(f.s.body(a).cpus);
        ac.ip = 300, ac.bx = 3, ac.cx = 64;
        f.s.run_cpu(f.s.body(a).cpus, 1); // claims the whole disk
        CHECK(f.s.disk_held(a) == 64);
        kill_now(f.s, a);
        CHECK(f.s.free_disk() == 64 && f.s.disk().stamp[0] == 128 && f.s.disk().stamp[63] == 128);
        f.op(100, 0, 3, 64); // the reader claims it: the only run there is
        CHECK(f.s.disk_held(f.b) == 64 && f.s.disk().stamp[10] == 128);
        f.op(100, 0, 0, 64); // a 64-codon block to load into
        const u32 to = f.cpu().wh;
        const u64 g0 = f.s.tally().garbled;
        f.op(101, 0, 2, 64); // DISK load
        const u64 garbled = f.s.tally().garbled - g0;
        CHECK(f.cpu().err == 0 && garbled > 35 && garbled < 60); // expected 48
        CHECK(f.s.stamp(to) == 0 && f.s.stamp(to + 63) == 0);
        CHECK(soup_ok(f.s));
    }
    // Snapshot resume with stamps equals running straight through; the rule
    // changes the world.
    {
        TextBuf cfg;
        cfg.append(kSoupWorld1b);
        cfg.append(kRule);
        Run straight;
        CHECK(make_run(straight, cfg.c_str(), 1));
        straight.run_until(80);
        Run first;
        CHECK(make_run(first, cfg.c_str(), 4));
        first.run_until(50);
        ByteWriter bytes;
        first.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
        CHECK_EQ_U64(resumed.world().hash(), first.world().hash());
        resumed.run_until(80);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
        Run off;
        CHECK(make_run(off, kSoupWorld1b, 4));
        off.run_until(80);
        CHECK(off.world().hash() != straight.world().hash());
    }
}

namespace {

// An emitter: channel 2, amplitude 64, once, then eight failing DIVIDEs halt it.
const char* const kEmitOnce = "seed.genome = ZERO INC INC ROTB ZERO INC SHL SHL SHL SHL SHL SHL EMIT "
                              "DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE DIVIDE\n";

// A five-node ring (no chords): every hop has latency 1.
void ring5(TextBuf& cfg, const char* extra) {
    cfg.append("seed = 4\nworld.model = soup\nworld.nodes = 5\nworld.chords = 0\nworld.soup_size = 512\n"
               "world.millicores = 1000\nseed.at = 100\nseed.millicores = 1000\nheat.enabled = false\n"
               "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\nsignal.channels = 4\n");
    cfg.append(kEmitOnce);
    cfg.append(extra);
}

} // namespace

// The signal channel.
void suite_signals() {
    // EMIT and LISTEN on one soup: off fails; on, emissions add up and cost
    // with loudness; LISTEN reads the field the world set.
    {
        DiskFix off;
        CHECK(off.init("EMIT LISTEN", 0));
        off.op(100, 0, 0, 5);
        CHECK(off.cpu().err == 1);
        off.op(101, 0, 0, 0);
        CHECK(off.cpu().err == 1);
        DiskFix f;
        CHECK(f.init("EMIT LISTEN", 0, "signal.channels = 4\n"));
        CHECK(f.op(100, 0, 1, 64) == 4 && f.cpu().err == 0); // 2 + 64/32
        f.op(100, 0, 1, static_cast<u32>(-10));
        CHECK(f.s.emissions()[1] == 54 && f.s.tally().emits == 2);
        f.op(100, 0, 4, 1); // no channel 4
        CHECK(f.cpu().err == 1);
        f.s.field_mut()[3] = -7;
        f.op(101, 0, 3, 0);
        CHECK(f.cpu().err == 0 && static_cast<i32>(f.cpu().cx) == -7 && f.s.tally().listens == 1);
    }
    // Propagation on a five-node ring, every node emitting 64 on channel 2 at
    // tick 0, with no floor: each node hears itself (64) at tick 1, its two
    // neighbours (32 + 32) at tick 2, the two nodes two hops away (16 + 16)
    // at tick 3, and then nothing: no echoes.
    {
        TextBuf cfg;
        ring5(cfg, "signal.attenuation = 0.5\nsignal.floor = 0\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 2));
        const World& w = run.world();
        CHECK(w.channels() == 4 && w.route_count(0) == 5);
        const i32 expect[5] = {0, 64, 64, 32, 0};
        bool ok = true;
        for (u32 t = 0; t < 5; ++t) {
            run.step();
            for (u32 n = 0; n < 5; ++n)
                ok = ok && w.node_soup(n).field()[2] == expect[t] && w.node_soup(n).field()[1] == 0;
        }
        CHECK(ok);
    }
    // One emitter; the floor drops what attenuation brings below it, and
    // every channel carries noise within the floor.
    {
        TextBuf cfg;
        ring5(cfg, "signal.attenuation = 0.5\nsignal.floor = 20\nseed.node = 0\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        const World& w = run.world();
        run.step(); // tick 0: node 0 emits
        run.step(); // tick 1: node 0 hears 64
        CHECK(w.node_soup(0).field()[2] >= 44 && w.node_soup(0).field()[2] <= 84);
        run.step(); // tick 2: neighbours 1 and 4 hear 32
        CHECK(w.node_soup(1).field()[2] >= 12 && w.node_soup(1).field()[2] <= 52);
        CHECK(w.node_soup(2).field()[2] >= -20 && w.node_soup(2).field()[2] <= 20);
        run.step(); // tick 3: 16 at two hops is below the floor: only noise
        bool quiet = true, noisy = false;
        for (u32 n = 0; n < 5; ++n)
            for (u32 c = 0; c < 4; ++c) {
                const i32 v = w.node_soup(n).field()[c];
                quiet = quiet && v >= -20 && v <= 20;
                noisy = noisy || v != 0;
            }
        CHECK(quiet && noisy);
    }
    // Paths in a procedural world: fewest hops, delay 1 + the latencies.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append("signal.channels = 2\nsignal.reach = 3\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        const World& w = run.world();
        bool ok = true, some = false;
        for (u32 n = 0; n < w.node_count(); ++n) {
            ok = ok && w.route_count(n) >= 1 && w.route_target(n, 0) == n && w.route_hops(n, 0) == 0 &&
                 w.route_delay(n, 0) == 1;
            for (u32 i = 1; i < w.route_count(n); ++i) {
                ok = ok && w.route_hops(n, i) >= 1 && w.route_hops(n, i) <= 3 &&
                     w.route_hops(n, i) >= w.route_hops(n, i - 1);
                if (w.route_hops(n, i) == 1) { // a neighbour: 1 + that edge's latency
                    const u32 m = w.route_target(n, i);
                    for (u32 e = w.out_begin(n); e < w.out_end(n); ++e)
                        if (w.edge_dst(e) == m)
                            ok = ok && w.route_delay(n, i) == 1 + w.edge_latency(e);
                    some = true;
                }
            }
        }
        CHECK(ok && some);
    }
    // Snapshot resume with signals in flight equals running straight through.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append("signal.channels = 4\n");
        Run straight;
        CHECK(make_run(straight, cfg.c_str(), 1));
        straight.run_until(60);
        Run first;
        CHECK(make_run(first, cfg.c_str(), 4));
        first.run_until(30);
        ByteWriter bytes;
        first.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
        CHECK_EQ_U64(resumed.world().hash(), first.world().hash());
        resumed.run_until(60);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    }
    // signaller: a gated lineage whose members emit on signal.sustained ticks.
    {
        TextBuf cfg;
        cfg.append("seed = 9\nworld.model = soup\nworld.soup_size = 16384\nworld.millicores = 4000\n"
                   "cpu.instr_per_core = 25000\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
                   "seed.at = 8000\nworld.nodes = 1\nsignal.channels = 4\nsignal.sustained = 3\n"
                   "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE\nseed.millicores = 10\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        run.run_until(20);
        CHECK(run.detection().found && !run.signaller().found);
        Soup& s = run.world().node_soup(0);
        const Lineages* L = run.world().node_lineages(0);
        u8 emit_codon = 0;
        for (u32 i = 0; i < 64; ++i)
            if (s.params().table.op[i] == Op::emit)
                emit_codon = static_cast<u8>(i);
        for (u32 round = 0; round < 3; ++round) {
            u32 member = kNone;
            for (u32 k = 0; k < s.block_slots() && member == kNone; ++k) {
                const BlockTag t = L->tag(k);
                if (t.lineage && s.block(k).body != kNone && L->lineage(t.lineage)->detected)
                    member = k;
            }
            CHECK(member != kNone);
            if (member == kNone)
                break;
            const u32 body = s.block(member).body;
            const u32 at = s.block(member).start;
            const u8 keep = s.mem()[at];
            s.mem_mut()[at] = emit_codon;
            Cpu& c = s.cpu_mut(s.body(body).cpus);
            c.ip = at, c.bx = 0, c.cx = 1, c.halted = 0;
            s.run_cpu(s.body(body).cpus, 1);
            s.mem_mut()[at] = keep;
            run.step();
        }
        CHECK(run.signaller().found);
        bool text = false;
        for (usize i = 0; i < run.event_count(); ++i)
            text = text || run.event_text(i).starts_with("signaller node=0 ");
        CHECK(text);
    }
    // The researcher's tap.
    {
        const char* csv = "nova_test_sig_stats.csv";
        const char* sig = "nova_test_signals.csv";
        TextBuf cfg;
        ring5(cfg, "signal.attenuation = 0.5\nsignal.floor = 0\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        TextBuf err;
        CHECK(run.open_stats(csv, 5, &err));
        CHECK(run.open_signals(sig, &err));
        run.run_until(5);
        run.finish();
        Array<u8> bytes;
        CHECK(read_file(sig, &bytes));
        bytes.push(0);
        const char* text = reinterpret_cast<const char*>(bytes.data());
        // (sound_amp: the disasters' own sounds on the channel, docs/disasters-plan.md X11)
        CHECK(strncmp(text, "tick_first,tick_last,channel,emits,amplitude,mean_field,sound_amp\n", 66) == 0);
        CHECK(strstr(text, "\n0,4,2,5,320,") != nullptr); // five emitters of 64 on channel 2
        remove(csv);
        remove(sig);
    }
}

namespace {

// A writer: claims a block, then 25 WRITEs into it; the IP wraps round the
// body, so it writes about 25 codons in every 32 instructions.
const char* const kWriter = "INC SHL SHL SHL SHL SHL ALLOC WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE "
                            "WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE WRITE "
                            "WRITE WRITE";

// The world-level invariants of a lifecycle world: edges only between live
// nodes, cargo only on edges that exist, every soup consistent.
bool lifecycle_ok(const World& w) {
    for (u32 e = 0; e < w.edge_count(); ++e)
        if (!w.live(w.edge_src(e)) || !w.live(w.edge_dst(e)))
            return false;
    for (u32 n = 0; n < w.node_count(); ++n) {
        if (!w.live(n) && (w.degree(n) != 0 || w.node_soup(n).body_count() != 0))
            return false;
        TextBuf why;
        if (!w.node_soup(n).check(&why)) {
            fprintf(stderr, "  lifecycle: node %u: %s\n", n, why.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

// The node lifecycle: wear and decline, starvation, health, collapse leaving
// remnants, ruins weathered on return, recovery and graph rebuilds.
void suite_lifecycle() {
    // Assembling the graph with every site live gives the generation's edges.
    {
        GraphParams gp;
        gp.nodes = 60;
        gp.ruggedness = 0.3;
        ProcGraph g;
        generate_graph(5, gp, &g);
        Array<u64> keys;
        keys.assign(g.keys.data(), g.keys.size());
        Array<u8> all;
        all.resize(60);
        all.fill(1);
        assemble_graph(5, gp, &g, all.data());
        bool same = keys.size() == g.keys.size();
        for (usize i = 0; same && i < keys.size(); ++i)
            same = keys[i] == g.keys[i];
        CHECK(same);
        all[7] = 0; // a dead site has no edges; cuts elsewhere don't move
        assemble_graph(5, gp, &g, all.data());
        bool none = true;
        for (usize i = 0; i < g.keys.size(); ++i)
            none = none && (g.keys[i] >> 32) != 7 && (g.keys[i] & 0xFFFFFFFFull) != 7;
        CHECK(none && g.keys.size() < keys.size());
    }
    // Wear: writes count, health falls, and the node is due to collapse at
    // its endurance.
    {
        DiskFix f;
        CHECK(f.init(kWriter, 0, "node.endurance = 1000\n"));
        CHECK(f.p.sel.sense_n == 18);
        u32 due_at = 0;
        for (u64 t = 0; t < 20 && !due_at; ++t) {
            f.s.tick(t);
            if (t == 0)
                CHECK(f.s.writes() > 100 && f.s.health() < 90 && f.s.health() > 50);
            if (f.s.collapse_due())
                due_at = static_cast<u32>(t);
        }
        CHECK(due_at >= 2 && f.s.writes() >= 1000 && f.s.health() == 0);
        // SENSE 17 reads the health.
        DiskFix g;
        CHECK(g.init("SENSE", 0, "node.endurance = 1000\n"));
        g.op(100, 0, 17, 0);
        CHECK(g.cpu().err == 0 && g.cpu().cx == 100);
    }
    // A full node doesn't collapse for being full (docs/dormancy-plan.md D7:
    // the starvation rule is gone): with wear far off, a node with all its
    // millicores held runs on, its health untouched.
    {
        DiskFix f;
        CHECK(f.init(kBusy, 0, "node.endurance = 1e12\n"));
        bool ever = false;
        for (u64 t = 0; t < 200; ++t) {
            f.s.tick(t);
            ever = ever || f.s.collapse_due();
        }
        CHECK(!ever && f.s.health() == 100);
    }
    // Collapse leaves remnants: every body dies, memory stays, stamped.
    {
        DiskFix f;
        CHECK(f.init(kBusy, 0, "node.endurance = 100000\nsalvage.ratio = 4\n"));
        u8 before[8];
        memcpy(before, f.s.mem() + 100, 8);
        f.s.collapse();
        CHECK(f.s.body_count() == 0 && memcmp(before, f.s.mem() + 100, 8) == 0 && f.s.stamp(100) == 8);
        CHECK(f.s.free_units() == 1000 && soup_ok(f.s));
        // Ruins: no fallow time, kept exactly; long fallow, scrambled.
        Ruins r;
        f.s.take_ruins(&r, 0);
        Soup back;
        back.init(&f.p, 3, 0, 1, &r);
        CHECK(memcmp(back.mem() + 100, before, 8) == 0 && back.stamp(100) == 8 && back.body_count() == 0);
        SoupParams rotten = f.p;
        rotten.rot_rate = 1e-3;
        f.s.take_ruins(&r, 100000); // 100 expected flips a codon: random
        Soup old;
        old.init(&rotten, 3, 0, 1, &r);
        u32 same = 0;
        for (u32 i = 0; i < 1024; ++i)
            same += old.mem()[i] == f.s.mem()[i];
        CHECK(same < 60); // ~16 by chance
        // A smaller node keeps the overlap.
        SoupParams small = f.p;
        small.size = 512;
        small.search_limit = small.contact_range = 400;
        small.spark_max = 128;
        f.s.take_ruins(&r, 0);
        Soup sm;
        sm.init(&small, 3, 0, 1, &r);
        CHECK(sm.size() == 512 && memcmp(sm.mem() + 100, before, 8) == 0);
    }
    // A world: nodes wear out, collapse, lie fallow, and come back.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append("node.endurance = 3000\nnode.fallow_min = 20\nnode.recover_every = 10\nsalvage.ratio = 4\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        const World& w = run.world();
        CHECK(w.lifecycle() && w.live_nodes() == 24);
        bool ok = true;
        u64 collapses = 0, recoveries = 0, max_live = 0, min_live = 99;
        for (u32 t = 0; t < 300; ++t) {
            run.step();
            collapses += w.last_soup_tick().collapses;
            recoveries += w.last_soup_tick().recoveries;
            max_live = max_of(max_live, w.live_nodes());
            min_live = min_of(min_live, w.live_nodes());
            ok = ok && lifecycle_ok(w);
        }
        CHECK(ok && collapses > 0 && recoveries > 0 && max_live <= 24);
        CHECK(w.collapses_total() == collapses && w.recoveries_total() == recoveries);
        usize col_ev = 0, rec_ev = 0;
        for (usize i = 0; i < run.event_count(); ++i) {
            col_ev += run.event_text(i).starts_with("collapse node=");
            rec_ev += run.event_text(i).starts_with("recovered node=");
        }
        CHECK(col_ev == collapses && rec_ev == recoveries);
        printf("  lifecycle: 24 nodes, endurance 3000: %llu collapses, %llu recoveries in 300 ticks (live %llu-%llu)\n",
               static_cast<unsigned long long>(collapses), static_cast<unsigned long long>(recoveries),
               static_cast<unsigned long long>(min_live), static_cast<unsigned long long>(max_live));

        // Snapshot resume through collapses and recoveries.
        Run straight;
        CHECK(make_run(straight, cfg.c_str(), 1));
        straight.run_until(400);
        ByteWriter bytes;
        run.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
        if (err.size())
            fprintf(stderr, "  lifecycle: %s\n", err.c_str());
        CHECK_EQ_U64(resumed.world().hash(), run.world().hash());
        resumed.run_until(400);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    }
    // Land below 1: some sites start as sea; the budget caps the live count.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append("world.land = 0.75\nworld.drift = 0.001\nnode.endurance = 3000\nnode.fallow_min = 20\n"
                   "node.recover_every = 10\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        const World& w = run.world();
        CHECK(w.live_nodes() == 18 && lifecycle_ok(w));
        bool ok = true;
        for (u32 t = 0; t < 200; ++t) {
            run.step();
            ok = ok && w.live_nodes() <= 18 && lifecycle_ok(w);
        }
        CHECK(ok && w.recoveries_total() > 0);
    }
}

// Transit corruption and link failures.
void suite_edges() {
    // Corruption: flips per cargo average edge.corruption x codons x ticks.
    {
        u64 flips = 0, changed = 0;
        const u32 runs = 200;
        for (u32 seed = 1; seed <= runs; ++seed) {
            TextBuf cfg;
            two_nodes(cfg, seed, "edge.corruption = 0.05\n");
            Run run;
            CHECK(make_run(run, cfg.c_str(), 1));
            const World& w = run.world();
            run.step(); // tick 0: she leaves
            for (u32 t = 1; t < 4; ++t) {
                run.step();
                flips += w.last_soup_tick().corrupted;
            }
            CHECK(w.last_soup_tick().arrived == 1);
            changed += w.last_soup_tick().corrupted > 0;
        }
        const double mean = static_cast<double>(flips) / runs; // expected 0.05 x 8 x 3 = 1.2
        CHECK(mean > 1.0 && mean < 1.4);
        CHECK(changed > runs / 2);
        printf("  edges: corruption 0.05 on an 8-codon, 3-tick crossing: %.2f flips a cargo (expected 1.2)\n",
               mean);
        // Off: the arrival is exactly the departure.
        TextBuf cfg;
        two_nodes(cfg, 1, "");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        run.run_until(4);
        const Soup& d = run.world().node_soup(1);
        CHECK(d.body_count() == 1 && run.world().last_soup_tick().corrupted == 0);
    }
    // A failure loses what the link carries, and the link stays down for its
    // downtime, both directions.
    {
        TextBuf cfg;
        two_nodes(cfg, 1, "edge.failure = 1\nedge.downtime = 20\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 1));
        const World& w = run.world();
        run.step(); // she leaves; at the barrier the link fails under her
        CHECK(w.last_soup_tick().sent == 1 && w.last_soup_tick().transit_lost == 1);
        CHECK(w.in_flight() == 0 && w.links_down() == 1 && w.edge_down(0) && w.edge_down(1));
        CHECK(w.edge_down_until(0) == w.edge_down_until(1) && w.edge_down_until(0) >= 2);
        run.run_until(10);
        CHECK(w.node_soup(1).body_count() == 0);
    }
    // Links come back after their downtime, and the state stays consistent.
    {
        TextBuf cfg;
        cfg.append(kSoupWorldProc);
        cfg.append("edge.failure = 0.01\nedge.downtime = 5\n");
        Run run;
        CHECK(make_run(run, cfg.c_str(), 4));
        const World& w = run.world();
        u64 lost = 0, max_down = 0, min_down = ~0ull;
        bool consistent = true;
        for (u32 t = 0; t < 200; ++t) {
            run.step();
            lost += w.last_soup_tick().transit_lost;
            max_down = max_of(max_down, w.links_down());
            if (t > 50)
                min_down = min_of(min_down, w.links_down());
            for (u32 e = 0; e < w.edge_count(); ++e) {
                if (!w.edge_down(e))
                    continue;
                bool rev = false;
                for (u32 f = w.out_begin(w.edge_dst(e)); f < w.out_end(w.edge_dst(e)); ++f)
                    if (w.edge_dst(f) == w.edge_src(e))
                        rev = w.edge_down(f) && w.edge_down_until(f) == w.edge_down_until(e);
                consistent = consistent && rev && w.edge_down_until(e) > w.tick();
            }
        }
        CHECK(consistent && max_down > 0 && max_down < w.edge_count() / 2);
        CHECK(lost > 0);
        printf("  edges: failure 0.01, downtime 5: up to %llu links down at once, %llu cargoes lost in 200 ticks\n",
               static_cast<unsigned long long>(max_down), static_cast<unsigned long long>(lost));

        // Snapshot resume with links down equals running straight through.
        Run straight;
        CHECK(make_run(straight, cfg.c_str(), 1));
        straight.run_until(260);
        ByteWriter bytes;
        run.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
        CHECK_EQ_U64(resumed.world().hash(), run.world().hash());
        resumed.run_until(260);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    }
    // A down link refuses DIVIDE; the others still work.
    {
        SoupParams p;
        CHECK(make_params(&p, "world.millicores = 1000\nworld.dispersal = true\nmem.upkeep = 0\n", 1024));
        Soup s;
        s.init(&p, 5, 0);
        u32 edges[2] = {0, 1};
        u32 in_flight[2] = {};
        u8 down[2] = {1, 0};
        DispersalPorts P;
        P.edges = edges;
        P.in_flight = in_flight;
        P.down = down;
        P.degree = 2;
        P.capacity = 2;
        s.set_ports(P);
        const u32 b = plant(s, 100, "ALLOC DIVIDE", 500);
        s.begin_tick(0);
        Cpu& c = s.cpu_mut(s.body(b).cpus);
        c.ip = 100, c.bx = 0, c.cx = 8;
        s.run_cpu(s.body(b).cpus, 1); // ALLOC 8
        c.ip = 101, c.bx = 1, c.cx = 0;
        s.run_cpu(s.body(b).cpus, 1); // to neighbour 1: down
        CHECK(c.err == 1 && s.departures().size() == 0);
        c.ip = 101, c.bx = 2, c.cx = 0;
        s.run_cpu(s.body(b).cpus, 1); // to neighbour 2: up
        CHECK(c.err == 0 && s.departures().size() == 1 && s.departures()[0].nbr == 1);
    }
}
