// Phase 10 (design.md 6.16, docs/phase10-plan.md): vents, decay, bites and
// death. INERT and nova_v2; dead matter decaying (memory, disk fossils,
// weathered ruins) while living code doesn't; vents writing only into waste
// near them; halted threads leaving their bodies; the frozen watchdog.
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "soup_helpers.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp: the defaults before Phase 10

namespace {

struct EcoFix {
    SoupParams p;
    Soup s;
    bool init(const char* extra, u32 size = 1024, const char* base = kQuiet) {
        TextBuf cfg;
        cfg.append(base);
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), size))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
};

u32 set_bits(const u8* m, u32 n, const Soup* s = nullptr) {
    u32 bits = 0;
    for (u32 a = 0; a < n; ++a) {
        if (s && s->owner_block(a) != kNone)
            continue;
        for (u32 b = 0; b < 6; ++b)
            bits += (m[a] >> b) & 1;
    }
    return bits;
}

// nova_v2: codon 0 is INERT, NOP0 keeps codon 1. INERT ends a template, and
// running it fails, so eight in a row halt a thread.
void test_inert() {
    CodonTable t;
    CHECK(builtin_table("nova_v2", &t));
    CHECK(t.op[0] == Op::inert && t.op[1] == Op::nop0 && t.op[2] == Op::nop1 && t.op[56] == Op::engulf);
    CHECK(strcmp(op_name(Op::inert), "INERT") == 0);
    EcoFix f;
    CHECK(f.init("isa.table = nova_v2\n"));
    CHECK(codon_of(f.p.table, Op::inert) == 0 && codon_of(f.p.table, Op::nop0) == 1);
    // JMP's template is one codon long: the INERT ends it, so it finds the NOP0 after.
    const u32 b = plant(f.s, 100, "JMP NOP1 INERT NOP0 INC", 100);
    CHECK(b != kNone);
    const u32 c = f.s.body(b).cpus;
    f.s.run_cpu(c, 1);
    CHECK(f.s.cpu(c).ip == 104 && f.s.cpu(c).err == 0);
    const u32 d = plant(f.s, 200, "INERT INERT INERT INERT INERT INERT INERT INERT INERT INERT", 100);
    const u32 dc = f.s.body(d).cpus;
    for (int i = 0; i < 7; ++i)
        f.s.run_cpu(dc, 1);
    CHECK(f.s.cpu(dc).err == 1 && !f.s.cpu(dc).halted);
    f.s.run_cpu(dc, 1);
    CHECK(f.s.cpu(dc).halted);
}

// Decay: waste loses bits toward INERT; living code and owned disk don't
// decay; free disk (fossils) does.
void test_decay() {
    EcoFix f;
    CHECK(f.init("isa.table = nova_v2\nmem.decay = 0.05\nworld.disk = 1024\ndisk.decay = 0.05\ndisk.rot = 0\n"));
    const u32 b = plant(f.s, 100, "ALLOC INC INC INC INC INC INC INC", 100);
    CHECK(b != kNone);
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.bx = 3; // ALLOC 16 codons of disk
    c.cx = 16;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.disk_held(b) == 16);
    c.bx = 99; // its ALLOCs fail from here on: it just loops
    u8 code[8], held[16];
    memcpy(code, f.s.mem() + 100, 8);
    Array<u32> addr;
    CHECK(f.s.disk().addresses(b, 0, 16, &addr));
    for (u32 i = 0; i < 16; ++i)
        held[i] = f.s.disk().mem[addr[i]];
    const u32 waste0 = set_bits(f.s.mem(), 1024, &f.s);
    u64 decayed = 0;
    for (u64 t = 1; t <= 100; ++t) {
        f.s.tick(t);
        decayed += f.s.tally().decayed;
    }
    CHECK(f.s.body(b).alive && memcmp(code, f.s.mem() + 100, 8) == 0);
    bool kept = true;
    for (u32 i = 0; i < 16; ++i)
        kept = kept && f.s.disk().mem[addr[i]] == held[i];
    CHECK(kept);
    const u32 waste1 = set_bits(f.s.mem(), 1024, &f.s);
    CHECK(waste1 * 5 < waste0 && decayed > 0); // about 5 losses a codon: about 9% of the bits left
    u32 free_bits = 0;
    for (u32 a = 0; a < 1024; ++a)
        if (f.s.disk().owner[a] == kNone)
            for (u32 k = 0; k < 6; ++k)
                free_bits += (f.s.disk().mem[a] >> k) & 1;
    CHECK(free_bits < 600); // of about 3,000
    CHECK(soup_ok(f.s));
}

// Ruins weather with decay over their fallow time: long enough, simply gone.
void test_ruins_fade() {
    SoupParams p;
    TextBuf cfg;
    cfg.append(kQuiet);
    cfg.append("isa.table = nova_v2\nmem.decay = 0.01\n");
    CHECK(make_params(&p, cfg.c_str(), 1024));
    Ruins r;
    r.mem.resize(1024);
    r.mem.fill(63);
    r.fallow = 1000; // 10 expected losses a codon
    Soup a;
    a.init(&p, 23, 0, 1, &r);
    CHECK(set_bits(a.mem(), 1024) == 0);
    r.fallow = 50; // half a loss a codon
    Soup b;
    b.init(&p, 23, 0, 1, &r);
    const u32 bits = set_bits(b.mem(), 1024);
    CHECK(bits < 6 * 1024 && bits > 5 * 1024);
}

// Vents write random codons only into waste within their radius; living
// code near a vent is untouched; the same seed gives the same vents and writes.
void test_vents() {
    EcoFix f, g;
    const char* v = "vent.spacing = 512\nvent.rate = 32\nvent.radius = 16\n";
    CHECK(f.init(v) && g.init(v));
    const Array<u32>& at = f.s.vent_places();
    CHECK(at.size() == 2 && g.s.vent_places().size() == 2 && at[0] == g.s.vent_places()[0]);
    for (u32 a = 0; a < 1024; ++a)
        f.s.mem_mut()[a] = g.s.mem_mut()[a] = 0;
    const u32 body_at = at[0] + 4 < 1024 - 8 ? at[0] + 4 : 100;
    const u32 b = plant(f.s, body_at, "INC INC INC INC INC INC INC INC", 100);
    plant(g.s, body_at, "INC INC INC INC INC INC INC INC", 100);
    CHECK(b != kNone);
    u8 code[8];
    memcpy(code, f.s.mem() + body_at, 8);
    u64 wrote = 0;
    for (u64 t = 1; t <= 5; ++t) {
        f.s.tick(t);
        g.s.tick(t);
        wrote += f.s.tally().vent_codons;
    }
    CHECK(wrote > 0 && memcmp(code, f.s.mem() + body_at, 8) == 0);
    bool near = true;
    u32 nonzero = 0;
    for (u32 a = 0; a < 1024; ++a) {
        if (f.s.owner_block(a) != kNone || f.s.mem()[a] == 0)
            continue;
        ++nonzero;
        bool in = false;
        for (usize k = 0; k < at.size(); ++k) {
            const u32 d = a > at[k] ? a - at[k] : at[k] - a;
            in = in || d <= 16 || 1024 - d <= 16;
        }
        near = near && in;
    }
    CHECK(near && nonzero > 0);
    CHECK(memcmp(f.s.mem(), g.s.mem(), 1024) == 0);
    CHECK(soup_ok(f.s));
}

// A halted thread leaves its body; with none left the body dies (halted).
void test_threads_end() {
    EcoFix f;
    CHECK(f.init("world.millicores = 2000\ncpu.threads = true\nlife.threads_end = true\n"));
    const u32 b = plant(f.s, 100, "GPU NOP1 NOP1 NOP1 ZERO INC INC INC NOP0 NOP0 NOP0 SWAP SWAP", 1000);
    const u32 c = f.s.body(b).cpus;
    f.s.cpu_mut(c).bx = kThreadOp;
    f.s.run_cpu(c, 1);
    const u32 t = f.s.cpu(c).next;
    CHECK(f.s.body(b).ncpu == 2 && t != kNone);
    f.s.cpu_mut(t).halted = 1;
    f.s.death_sweep();
    CHECK(f.s.body(b).alive && f.s.body(b).ncpu == 1 && f.s.cpu(c).next == kNone);
    CHECK(f.s.tally().threads_ended == 1 && f.s.tally().died_halted == 0);
    CHECK(soup_ok(f.s));
    f.s.cpu_mut(c).halted = 1;
    f.s.death_sweep();
    CHECK(!f.s.body(b).alive && f.s.tally().died_halted == 1 && f.s.body_count() == 0);
    CHECK(soup_ok(f.s));
    // Off: the halted thread stays.
    EcoFix g;
    CHECK(g.init("world.millicores = 2000\ncpu.threads = true\n"));
    const u32 b2 = plant(g.s, 100, "GPU NOP1 NOP1 NOP1 ZERO INC INC INC NOP0 NOP0 NOP0 SWAP SWAP", 1000);
    const u32 c2 = g.s.body(b2).cpus;
    g.s.cpu_mut(c2).bx = kThreadOp;
    g.s.run_cpu(c2, 1);
    g.s.cpu_mut(g.s.cpu(c2).next).halted = 1;
    g.s.death_sweep();
    CHECK(g.s.body(b2).ncpu == 2 && g.s.tally().threads_ended == 0);
}

// The frozen watchdog: a body deep in upkeep debt runs nothing and dies after
// life.frozen_ticks; a running body, and one waiting on the disk, don't.
void test_frozen() {
    EcoFix f;
    CHECK(f.init("life.frozen_ticks = 5\n", 1024,
                 "heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 1000000\n"));
    const u32 b = plant(f.s, 100, "INC INC INC INC", 1000);
    for (u64 t = 0; t < 5; ++t)
        f.s.tick(t);
    CHECK(f.s.body(b).alive && f.s.cpu(f.s.body(b).cpus).executed == 0);
    f.s.tick(5);
    CHECK(!f.s.body(b).alive && f.s.tally().died_frozen == 1);
    CHECK(soup_ok(f.s));
    EcoFix g; // running
    CHECK(g.init("life.frozen_ticks = 5\n"));
    const u32 b2 = plant(g.s, 100, "INC INC INC INC", 1000);
    for (u64 t = 0; t < 20; ++t)
        g.s.tick(t);
    CHECK(g.s.body(b2).alive && g.s.tally().died_frozen == 0);
    // Waiting on the disk (a store of about 67 ticks) isn't frozen.
    EcoFix h;
    CHECK(h.init("life.frozen_ticks = 3\nworld.disk = 4096\ncost.table = home_v1\ncpu.instr_per_core = 1000\n"));
    const u32 b3 = plant(h.s, 100, "ALLOC DISK INC INC INC INC", 1000);
    Cpu& c = h.s.cpu_mut(h.s.body(b3).cpus);
    c.bx = 3;
    c.cx = 16;
    h.s.run_cpu(h.s.body(b3).cpus, 1);
    c.bx = 1;
    c.ax = 0;
    c.cx = 16;
    c.rh = 100;
    h.s.run_cpu(h.s.body(b3).cpus, 100000000);
    CHECK(h.s.actions().size() == 1 && h.s.actions()[0].end > 20);
    for (u64 t = 1; t <= 20; ++t)
        h.s.tick(t);
    CHECK(h.s.body(b3).alive && h.s.actions().size() == 1 && h.s.tally().died_frozen == 0);
    CHECK(soup_ok(h.s));
}

// A bite of raw material: the nearest waste, up to CX codons and at most the
// eater's own size, claimed as its latest block with its contents.
void test_bite_waste() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\n"));
    u8 before[3];
    memcpy(before, f.s.mem() + 97, 3);
    const u32 b = plant(f.s, 100, "ENGULF INC INC", 100);
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.cx = 3;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    // ip 100: the nearest waste is 99; the run through it, backwards, is 97-99.
    CHECK(c.err == 0 && f.s.body(b).mem == 6 && f.s.owner_body(97) == b && f.s.owner_body(99) == b);
    CHECK(memcmp(before, f.s.mem() + 97, 3) == 0 && f.s.block(f.s.body(b).latest).start == 97);
    CHECK(f.s.tally().bites_waste == 1 && f.s.tally().codons_bitten == 3 && f.s.tally().engulfs == 0);
    c.ip = 100;
    c.cx = 1000; // capped at its own size, 6
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.body(b).mem == 12 && f.s.tally().codons_bitten == 9);
    c.ip = 100;
    c.cx = 0; // bites nothing
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(c.err == 1 && f.s.body(b).mem == 12);
    CHECK(soup_ok(f.s));
}

// Aimed bites: a body no bigger than the bite is swallowed whole; a bigger
// one loses its end nearest the eater and the same share of its millicores.
void test_bite_body() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\nworld.millicores = 4000\n"));
    const u32 e = plant(f.s, 100, "ENGULF NOP0 INC INC INC INC INC INC INC INC", 500);
    const u32 small = plant(f.s, 150, "NOP1 INC INC INC", 300);
    CHECK(e != kNone && small != kNone);
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 10;
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && !f.s.body(small).alive && f.s.body(e).units == 800 && f.s.tally().engulfs == 1);
    f.s.death_sweep();
    // A bigger prey: 40 codons, 400 millicores. The eater (now 14 codons)
    // bites 10 from its low end, nearest the eater, and a quarter of its units.
    TextBuf big;
    big.append("NOP1");
    for (int i = 0; i < 39; ++i)
        big.append(" INC");
    const u32 prey = plant(f.s, 200, big.c_str(), 400);
    CHECK(prey != kNone);
    c.ip = 100;
    c.cx = 10;
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && f.s.body(prey).alive && f.s.body(prey).mem == 30 && f.s.body(e).mem == 24);
    CHECK(f.s.owner_body(200) == e && f.s.owner_body(209) == e && f.s.owner_body(210) == prey);
    CHECK(f.s.block(f.s.body(prey).blocks).start == 210); // its genome now starts past the bite
    CHECK(f.s.body(prey).units == 300 && f.s.body(e).units == 900 && f.s.tally().bites_body == 1);
    CHECK(soup_ok(f.s));
    // The prey's thread was running in the bitten code: it fails and halts.
    const u32 pc = f.s.body(prey).cpus;
    CHECK(f.s.cpu(pc).ip == 200);
    for (int i = 0; i < 8; ++i)
        f.s.run_cpu(pc, 1);
    CHECK(f.s.cpu(pc).halted);
    // From the far side: an eater past the prey's end bites its high end.
    EcoFix g;
    CHECK(g.init("engulf.bite = true\nworld.millicores = 4000\n"));
    const u32 prey2 = plant(g.s, 200, big.c_str(), 400);
    const u32 e2 = plant(g.s, 300, "ENGULF NOP0 INC INC INC INC INC INC INC INC", 500);
    Cpu& c2 = g.s.cpu_mut(g.s.body(e2).cpus);
    c2.cx = 10;
    g.s.run_cpu(g.s.body(e2).cpus, 1);
    CHECK(c2.err == 0 && g.s.owner_body(239) == e2 && g.s.owner_body(230) == e2 && g.s.owner_body(229) == prey2);
    CHECK(g.s.block(g.s.body(prey2).blocks).start == 200 && g.s.body(prey2).mem == 30);
    CHECK(soup_ok(g.s));
}

// A surface shields a body from blind bites: the blind eater takes the
// nearest waste instead, past the shielded body.
void test_bite_shield() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\n"));
    CHECK(plant(f.s, 90, "NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1", 100) != kNone);
    const u32 e = plant(f.s, 100, "ENGULF INC INC INC", 100);
    const u32 prey = plant(f.s, 104, "NOP1 INC INC INC", 100);
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 4;
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && f.s.body(prey).mem == 4 && f.s.owner_body(108) == e && f.s.tally().bites_waste == 1);
    CHECK(soup_ok(f.s));
}

// With a timed table a bite is an action: its codons at
// engulf.codons_per_tick, the CPU busy meanwhile, landing at its end.
void test_bite_action() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\ncost.table = home_v1\nworld.millicores = 4000\nengulf.codons_per_tick = 8\n"));
    TextBuf big;
    big.append("NOP1");
    for (int i = 0; i < 39; ++i)
        big.append(" INC");
    const u32 prey = plant(f.s, 200, big.c_str(), 400);
    TextBuf eat;
    eat.append("ENGULF NOP0");
    for (int i = 0; i < 18; ++i)
        eat.append(" INC");
    const u32 e = plant(f.s, 100, eat.c_str(), 500);
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 20;
    CHECK(f.s.run_cpu(f.s.body(e).cpus, 1000000) == 1000000); // the rest of the tick goes on the bite
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].end == 3 && f.s.actions()[0].bite == 20); // 20 codons at 8
    CHECK(!f.s.cpu_waits(f.s.body(e).cpus));
    f.s.begin_tick(3);
    f.s.run_cpu(f.s.body(e).cpus, 0);
    CHECK(f.s.tally().bites_body == 1 && f.s.actions().empty() && f.s.body(prey).mem == 20);
    // A blind bite (no template after ENGULF now) takes raw material: the
    // nearest waste, at 99.
    c.ip = 100;
    c.cx = 5;
    f.s.mem_mut()[101] = static_cast<u8>(codon_of(f.p.table, Op::inc));
    f.s.run_cpu(f.s.body(e).cpus, 1000000);
    CHECK(f.s.actions().size() == 1 && f.s.actions()[0].target == kNone && f.s.actions()[0].end == 4);
    f.s.begin_tick(4);
    f.s.run_cpu(f.s.body(e).cpus, 0);
    CHECK(f.s.tally().bites_waste == 1 && f.s.actions().empty());
    CHECK(soup_ok(f.s));
}

// A busy timed world with everything of Phase 10 on: the determinism gates.
const char* const kEcoWorld = "seed = 42\n"
                              "world.model = soup\n"
                              "world.nodes = 24\n"
                              "world.chords = 12\n"
                              "world.soup_size = 2048\n"
                              "world.millicores = 400\n"
                              "cpu.instr_per_core = 25000\n"
                              "cpu.wear = 0.002\n"
                              "isa.table = nova_v2\n"
                              "spark.timing = last\n"
                              "spark.initial = 20\n"
                              "spark.rate = 0.25\n"
                              "spark.grant = 20\n"
                              "spark.min_len = 8\n"
                              "spark.max_len = 24\n"
                              "seed.genome = NOP0 SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE INJECT NOP1 DIVIDE\n"
                              "seed.at = 1000\n"
                              "seed.millicores = 100\n"
                              "hazard.copy = 0.005\n"
                              "hazard.flaw = 0.001\n"
                              "hazard.rot = 0.0000005\n"
                              "heat.cpu_gain = 36\n"
                              "cost.table = home_v1\n"
                              "cpu.threads = true\n"
                              "cpu.e_share = 0.5\n"
                              "mem.decay = 0.0003\n"
                              "vent.spacing = 1024\n"
                              "engulf.bite = true\n"
                              "engulf.codons_per_tick = 8\n"
                              "life.threads_end = true\n"
                              "life.frozen_ticks = 200\n";

void test_eco_world() {
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, kEcoWorld, threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = n - 1 - i;
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 300; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = run.world().last_soup_tick();
                sum.vent_codons += s.vent_codons;
                sum.decayed += s.decayed;
                sum.bites_waste += s.bites_waste;
                sum.bites_body += s.bites_body;
                sum.engulfs += s.engulfs;
                sum.threads_ended += s.threads_ended;
                sum.died_frozen += s.died_frozen;
                sum.died_halted += s.died_halted;
                sum.died_worn += s.died_worn;
            }
        }
        hashes[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    CHECK_EQ_U64(hashes[1], hashes[0]);
    CHECK_EQ_U64(hashes[2], hashes[0]);
    CHECK_EQ_U64(hashes[3], hashes[0]);
    CHECK(sum.vent_codons > 0 && sum.decayed > 0 && sum.bites_waste > 0 && sum.threads_ended > 0);
    Run a;
    CHECK(make_run(a, kEcoWorld, 4));
    a.run_until(150);
    ByteWriter snap;
    a.snapshot(snap);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(snap.data(), snap.size(), 2, &err));
    b.run_until(300);
    CHECK_EQ_U64(b.world().hash(), hashes[0]);
    printf("  eco world: 300 ticks, vents %llu codons, decayed %llu bits, bites %llu of waste and %llu of bodies, "
           "%llu swallowed, threads ended %llu, deaths halted %llu frozen %llu worn %llu, hash %016llx\n",
           static_cast<unsigned long long>(sum.vent_codons), static_cast<unsigned long long>(sum.decayed),
           static_cast<unsigned long long>(sum.bites_waste), static_cast<unsigned long long>(sum.bites_body),
           static_cast<unsigned long long>(sum.engulfs), static_cast<unsigned long long>(sum.threads_ended),
           static_cast<unsigned long long>(sum.died_halted), static_cast<unsigned long long>(sum.died_frozen),
           static_cast<unsigned long long>(sum.died_worn), static_cast<unsigned long long>(hashes[0]));
}

// C8: with no fallback (as in a real run), every feature is on by default.
// The suites run on the old defaults (test_main.cpp), so this clears them for
// the test and puts them back.
void test_defaults() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    SoupParams p;
    const bool made = make_params(&p, "", 16384);
    CHECK(made);
    if (made) {
        CHECK(Str(p.table.name).eq("nova_v2") && Str(p.cost.name).eq("home_v1"));
        CHECK(p.threads && p.e_share == 0.5 && p.cpu_wear == 0.001);
        CHECK(p.gpus == 400 && p.npus == 200 && p.warps && p.spark_gpu_grant == 8 && p.accel_async);
        CHECK(p.disk == 4096 && p.salvage_ratio == 4.0 && p.signal_channels == 16 && p.volatility == 0.00001);
        CHECK(p.ambient == 0.05f && p.endurance == 0 && p.starve_ticks == 0); // (no lifecycle without a map)
        CHECK(p.mem_decay > 0 && p.rot_rate > 0 && p.rot_rate < p.mem_decay * 0.011 && p.rot_rate > p.mem_decay * 0.009);
        CHECK(p.disk_decay == 1e-6 && p.disk_rot == 1e-8);
        CHECK(p.vent_spacing == 4096 && p.vent_rate == 64 && p.vent_radius == 256);
        CHECK(p.bites && p.engulf_cpt == 8 && p.threads_end && p.frozen_ticks == 200);
    }
    // A small world on every default: deterministic at 1 and 4 threads.
    const char* w = "world.model = soup\nworld.nodes = 4\nworld.soup_size = 4096\nspark.initial = 40\n"
                    "spark.rate = 0.25\n";
    u64 h[2] = {};
    const u32 threads[2] = {1, 4};
    for (u32 v = 0; v < 2; ++v) {
        Run run;
        CHECK(make_run(run, w, threads[v]));
        run.run_until(100);
        h[v] = run.world().hash();
        if (v == 0)
            for (u32 n = 0; n < run.world().node_count(); ++n)
                CHECK(soup_ok(run.world().node_soup(n)));
    }
    CHECK_EQ_U64(h[1], h[0]);
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    printf("  defaults world: 100 ticks, hash %016llx\n", static_cast<unsigned long long>(h[0]));
}

} // namespace

void suite_ecology() {
    test_inert();
    test_decay();
    test_ruins_fade();
    test_vents();
    test_threads_end();
    test_frozen();
    test_bite_waste();
    test_bite_body();
    test_bite_shield();
    test_bite_action();
    test_eco_world();
    test_defaults();
}
