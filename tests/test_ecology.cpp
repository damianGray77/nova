// Phase 10 (design.md 6.16, docs/phase10-plan.md): vents, decay, bites and
// death. INERT and nova_v2; dead matter decaying (memory, disk fossils,
// weathered ruins) while living code doesn't; vents writing only into waste
// near them; halted threads leaving their bodies; the frozen watchdog.
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/lineage.h"
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
    // (bleeding off: this is decay alone, and the planted body's ends are open)
    CHECK(f.init("isa.table = nova_v2\nmem.decay = 0.05\nworld.disk = 1024\ndisk.decay = 0.05\ndisk.rot = 0\n"
                 "life.bleed_every = 0\n"));
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

// A bite of raw material: the nearest waste, up to CX codons and at most a
// third of the eater's own size, rounded up (docs/bites-plan.md B2), claimed
// as its latest block with its contents.
void test_bite_waste() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\n"));
    u8 before[3];
    memcpy(before, f.s.mem() + 97, 3);
    const u32 b = plant(f.s, 100, "ENGULF INC INC", 100);
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.cx = 3;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    // ip 100: the nearest waste is 99; a third of 3 codons is 1.
    CHECK(c.err == 0 && f.s.body(b).mem == 4 && f.s.owner_body(99) == b && f.s.owner_body(98) != b);
    CHECK(f.s.mem()[99] == before[2] && f.s.block(f.s.body(b).latest).start == 99);
    CHECK(f.s.tally().bites_waste == 1 && f.s.tally().codons_bitten == 1 && f.s.tally().engulfs == 0);
    c.ip = 100;
    c.cx = 1000; // a third of 4, rounded up: 2 (97-98)
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.body(b).mem == 6 && f.s.tally().codons_bitten == 3);
    CHECK(memcmp(before, f.s.mem() + 97, 3) == 0);
    c.ip = 100;
    c.cx = 0; // bites nothing
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(c.err == 1 && f.s.body(b).mem == 6);
    CHECK(soup_ok(f.s));
    // engulf.bite_share = 1: up to its whole size, as bites were before.
    EcoFix g;
    CHECK(g.init("engulf.bite = true\nengulf.bite_share = 1\n"));
    const u32 b2 = plant(g.s, 100, "ENGULF INC INC", 100);
    g.s.cpu_mut(g.s.body(b2).cpus).cx = 3;
    g.s.run_cpu(g.s.body(b2).cpus, 1);
    CHECK(g.s.body(b2).mem == 6 && g.s.tally().codons_bitten == 3);
}

// Aimed bites: a body no bigger than the bite is swallowed whole; a bigger
// one loses its end nearest the eater, and its cores in proportion to the
// code taken, its NOPs not counted (docs/bites-plan.md B4: NOPs are tissue).
void test_bite_body() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\nworld.millicores = 4000\n"));
    const u32 e = plant(f.s, 100, "ENGULF NOP0 INC INC INC INC INC INC INC INC", 500);
    const u32 small = plant(f.s, 150, "NOP1 INC INC INC", 300);
    CHECK(e != kNone && small != kNone);
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 10; // a third of 10, rounded up: 4, the whole small prey
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && !f.s.body(small).alive && f.s.body(e).units == 800 && f.s.tally().engulfs == 1);
    f.s.death_sweep();
    // A bigger prey: 40 codons (a NOP and 39 INC), 400 millicores. The eater
    // (now 14 codons) bites 5 from its low end, nearest the eater: a NOP and 4
    // of its 39 codons of code, so 41 of its 400 millicores.
    TextBuf big;
    big.append("NOP1");
    for (int i = 0; i < 39; ++i)
        big.append(" INC");
    const u32 prey = plant(f.s, 200, big.c_str(), 400);
    CHECK(prey != kNone);
    c.ip = 100;
    c.cx = 10;
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && f.s.body(prey).alive && f.s.body(prey).mem == 35 && f.s.body(e).mem == 19);
    CHECK(f.s.owner_body(200) == e && f.s.owner_body(204) == e && f.s.owner_body(205) == prey);
    CHECK(f.s.block(f.s.body(prey).blocks).start == 205); // its genome now starts past the bite
    CHECK(f.s.body(prey).units == 359 && f.s.body(e).units == 841 && f.s.tally().bites_body == 1);
    CHECK(f.s.tally().wounds == 1 && f.s.open_end(prey, false)); // its low end is code now: a wound
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
    CHECK(c2.err == 0 && g.s.owner_body(239) == e2 && g.s.owner_body(236) == e2 && g.s.owner_body(235) == prey2);
    CHECK(g.s.block(g.s.body(prey2).blocks).start == 200 && g.s.body(prey2).mem == 36);
    CHECK(soup_ok(g.s));
    // Tissue: a bite of padding alone takes no cores and leaves no wound.
    EcoFix h;
    CHECK(h.init("engulf.bite = true\nworld.millicores = 4000\n"));
    const u32 padded = plant(h.s, 200, "NOP0 NOP0 NOP0 NOP0 NOP0 NOP0 INC INC INC INC INC INC INC INC INC INC INC NOP0", 400);
    const u32 e3 = plant(h.s, 100, "ENGULF NOP1 INC INC INC INC INC INC INC INC", 500);
    Cpu& c3 = h.s.cpu_mut(h.s.body(e3).cpus);
    c3.cx = 10; // 4 codons: four of the six NOPs
    h.s.run_cpu(h.s.body(e3).cpus, 1);
    CHECK(c3.err == 0 && h.s.body(padded).mem == 14 && h.s.body(padded).units == 400 && h.s.body(e3).units == 500);
    CHECK(h.s.tally().wounds == 0 && !h.s.open_end(padded, false));
    CHECK(soup_ok(h.s));
}

// docs/bites-plan.md B1: a surface no longer shields a body from blind
// bites. The blind eater takes the nearest material -- here the surfaced
// prey, nearer than any waste -- and leaves it wounded, its low end now code.
void test_bite_shield() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\n"));
    CHECK(plant(f.s, 90, "NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1", 100) != kNone);
    const u32 e = plant(f.s, 100, "ENGULF INC INC INC", 100);
    const u32 prey = plant(f.s, 104, "NOP1 INC INC INC", 100);
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 4; // a third of 4, rounded up: 2
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && f.s.body(prey).mem == 2 && f.s.owner_body(104) == e && f.s.owner_body(105) == e);
    CHECK(f.s.owner_body(106) == prey && f.s.tally().bites_body == 1 && f.s.tally().bites_waste == 0);
    CHECK(f.s.tally().wounds == 1 && f.s.open_end(prey, false));
    CHECK(soup_ok(f.s));
}
// Dormancy (docs/dormancy-plan.md): a frozen body with a reserve (here, disk)
// goes dormant instead of dying -- its cores and disk back to the node, its
// code kept -- and with nowhere to go rests here, waking when a core of its
// kind is free (its own freed ones); woken, it holds its code and cores again.
// A bare frozen body dies as before. The clock spends its reserve and kills
// it when the reserve is gone.
void test_dormant() {
    const char* base = "heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 1000000\n"
                       "world.millicores = 2000\nworld.disk = 4096\nlife.frozen_ticks = 5\nlife.dormancy = true\n";
    EcoFix f;
    CHECK(f.init("", 1024, base));
    const u32 b = plant(f.s, 100, "ALLOC INC INC INC", 1000);
    const u32 bare = plant(f.s, 300, "INC INC INC INC", 500);
    Cpu& c = f.s.cpu_mut(f.s.body(b).cpus);
    c.bx = 3; // disk
    c.cx = 16;
    f.s.run_cpu(f.s.body(b).cpus, 1);
    CHECK(f.s.disk_held(b) == 16);
    for (u64 t = 0; t <= 5; ++t)
        f.s.tick(t);
    CHECK(f.s.body(b).alive && f.s.dormant(b) && f.s.body(b).units == 0 && f.s.body(b).cpus == kNone);
    CHECK(f.s.body(b).mem == 4 && f.s.disk_held(b) == 0 && f.s.dormant_state(b).disk_reserve == 16);
    CHECK(f.s.dormant_state(b).millicores == 1000 && f.s.free_units() == f.s.params().millicores);
    CHECK(f.s.tally().dormant_started == 1 && f.s.frozen_bare() == 1 && !f.s.body(bare).alive);
    CHECK(soup_ok(f.s));
    f.s.tick(6); // its own freed millicores are free: it wakes, with them and its disk
    CHECK(f.s.body(b).alive && !f.s.dormant(b) && f.s.body(b).units == 1000 && f.s.body(b).cpus != kNone);
    CHECK(f.s.body(b).mem == 4 && f.s.disk_held(b) == 16 && f.s.tally().dormant_woke == 1);
    CHECK(soup_ok(f.s));
    // The clock: with every millicore held elsewhere it can't wake, and its
    // reserve runs out (8 disk codons a tick: 16 in 2 ticks).
    EcoFix g;
    TextBuf gb;
    gb.append(base);
    gb.append("dormant.disk_loss = 8\n");
    CHECK(g.init("", 1024, gb.c_str()));
    const u32 d = plant(g.s, 100, "ALLOC INC INC INC", 1000);
    Cpu& dc = g.s.cpu_mut(g.s.body(d).cpus);
    dc.bx = 3;
    dc.cx = 16;
    g.s.run_cpu(g.s.body(d).cpus, 1);
    CHECK(g.s.force_dormant(d) && g.s.dormant(d));
    const u32 hog = plant(g.s, 400, "INC INC INC INC", g.s.params().millicores);
    CHECK(hog != kNone && g.s.free_units() == 0);
    g.s.begin_tick(1);
    g.s.death_sweep();
    CHECK(g.s.dormant(d) && g.s.dormant_state(d).disk_reserve == 8);
    g.s.begin_tick(2);
    g.s.death_sweep();
    CHECK(!g.s.body(d).alive && g.s.tally().dormant_expired == 1);
    CHECK(soup_ok(g.s));
}

// The lineage tracker and dormancy (disasters plan X12). Two members of one
// lineage on a soup with a detector: a (the founder, at 100) and b, its exact
// copy written by a's body (at 300, depth 1), which holds 16 codons of disk so
// it can go dormant.
struct LineageFix {
    SoupParams p;
    DetectParams d;
    Lineages L;
    Soup s;
    u32 a = kNone, b = kNone;
    u64 id = 0;
    bool init(const char* extra = "") {
        TextBuf cfg;
        cfg.append("heat.enabled = false\nhazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
                   "world.millicores = 2000\nworld.disk = 4096\nlife.dormancy = true\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        d.enabled = true;
        s.init(&p, 23, 0);
        L.init(&d, 0);
        s.set_observer(&L);
        s.begin_tick(0);
        a = plant(s, 100, "ALLOC INC INC INC", 500);
        b = plant(s, 300, "ALLOC INC INC INC", 500);
        if (a == kNone || b == kNone)
            return false;
        for (u32 i = 0; i < 4; ++i)
            L.on_write(300 + i);
        L.on_divide(s, s.body(a).blocks, s.body(b).blocks, 0);
        id = L.tag(s.body(a).blocks).lineage;
        Cpu& c = s.cpu_mut(s.body(b).cpus);
        c.bx = 3; // disk
        c.cx = 16;
        s.run_cpu(s.body(b).cpus, 1);
        return id != 0 && L.tag(s.body(b).blocks).lineage == id && alive() == 2 && s.disk_held(b) == 16;
    }
    u32 alive() const {
        const LineageInfo* i = L.lineage(id);
        return i ? i->alive : 0;
    }
    usize ended() const { // rows for this lineage among the detector's extinct ones
        usize n = 0;
        for (usize i = 0; i < L.ends().size(); ++i)
            n += L.ends()[i].id == id;
        return n;
    }
};

// A dormant program leaves its lineage when it goes dormant and rejoins when
// it wakes: resting and waking at home (a hermit node: nowhere to drift)
// leaves the lineage's count where it was, round after round (it used to gain
// a phantom member each time), and the lineage ends when its last member dies.
// One that drifts off, or dies resting, leaves it once, not twice.
void test_dormant_lineage() {
    LineageFix f;
    CHECK(f.init());
    for (u64 t = 1; t <= 3; ++t) {
        CHECK(f.s.force_dormant(f.b) && f.s.dormant(f.b)); // nowhere to drift: it rests here
        CHECK(f.alive() == 1 && f.L.tag(f.s.body(f.b).blocks).lineage == 0);
        f.s.begin_tick(t);
        f.s.death_sweep(); // its own freed millicores are free: it wakes
        CHECK(!f.s.dormant(f.b) && f.s.body(f.b).alive && f.s.tally().dormant_woke == 1);
        const BlockTag tb = f.L.tag(f.s.body(f.b).blocks);
        CHECK(f.alive() == 2 && tb.lineage == f.id && tb.depth == 1);
        CHECK(f.s.disk_held(f.b) == 16); // its reserve again
        CHECK(soup_ok(f.s));
    }
    const LineageInfo* info = f.L.lineage(f.id);
    CHECK(info && info->peak_alive == 2 && f.ended() == 0);
    // The last members die: the lineage ends, once.
    f.s.cpu_mut(f.s.body(f.a).cpus).halted = 1;
    f.s.cpu_mut(f.s.body(f.b).cpus).halted = 1;
    f.s.begin_tick(4);
    f.s.death_sweep();
    CHECK(!f.s.body(f.a).alive && !f.s.body(f.b).alive);
    CHECK(f.L.lineage(f.id) == nullptr && f.L.living() == 0 && f.ended() == 1);
    CHECK(soup_ok(f.s));

    // Drifting off: it leaves its lineage once (going dormant), and its
    // departure, which frees its block, leaves nothing more. Its tag goes with it.
    LineageFix g;
    CHECK(g.init());
    const u32 edges[1] = {0};
    const u32 in_flight[1] = {0};
    DispersalPorts ports;
    ports.edges = edges;
    ports.in_flight = in_flight;
    ports.degree = 1;
    ports.capacity = 2;
    g.s.set_ports(ports);
    CHECK(g.s.force_dormant(g.b) && !g.s.dormant(g.b) && g.s.departures().size() == 1);
    CHECK(g.alive() == 1 && g.L.living() == 1 && g.ended() == 0);
    const CargoTag t = g.s.departures()[0].tag;
    CHECK(t.lineage == g.id && t.depth == 1 && g.s.departures()[0].dormant == 1);

    // Dying while it rests: it isn't a member then, so its death changes nothing.
    LineageFix h;
    CHECK(h.init("dormant.disk_loss = 8\n"));
    CHECK(h.s.force_dormant(h.b) && h.s.dormant(h.b) && h.alive() == 1);
    CHECK(plant(h.s, 600, "INC INC INC INC", h.s.free_units()) != kNone && h.s.free_units() == 0); // it can't wake
    h.s.begin_tick(1);
    h.s.death_sweep();
    h.s.begin_tick(2);
    h.s.death_sweep(); // its reserve spent
    CHECK(!h.s.body(h.b).alive && h.s.tally().dormant_expired == 1);
    CHECK(h.alive() == 1 && h.ended() == 0);
    // ... and if it was the last member, the lineage ended when it went dormant.
    h.s.cpu_mut(h.s.body(h.a).cpus).halted = 1;
    h.s.begin_tick(3);
    h.s.death_sweep();
    CHECK(h.L.lineage(h.id) == nullptr && h.ended() == 1);
    CHECK(soup_ok(h.s));
}

// A resting dormant program is easy food: swallowed whole it is gone. (No
// surface shields a body from a blind bite now, docs/bites-plan.md B1; the
// eater is big enough that a third of it covers the prey.)
void test_dormant_bitten() {
    EcoFix f;
    CHECK(f.init("engulf.bite = true\nworld.disk = 4096\nlife.dormancy = true\n"));
    CHECK(plant(f.s, 90, "NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1 NOP1", 100) != kNone);
    const u32 prey = plant(f.s, 104, "NOP1 ALLOC INC INC", 100);
    const u32 e = plant(f.s, 108, "ENGULF INC INC INC INC INC INC INC INC INC INC INC", 100);
    Cpu& pc = f.s.cpu_mut(f.s.body(prey).cpus);
    pc.ip = 105;
    pc.bx = 3;
    pc.cx = 16;
    f.s.run_cpu(f.s.body(prey).cpus, 1);
    CHECK(f.s.force_dormant(prey) && f.s.dormant(prey));
    Cpu& c = f.s.cpu_mut(f.s.body(e).cpus);
    c.cx = 4;
    f.s.run_cpu(f.s.body(e).cpus, 1);
    CHECK(c.err == 0 && !f.s.body(prey).alive && f.s.owner_body(104) == e);
    CHECK(f.s.tally().engulfs == 1 && f.s.tally().dormant_eaten == 1);
    f.s.death_sweep();
    CHECK(soup_ok(f.s));
}
// Dormancy in a world: frozen bodies go dormant, drift between nodes and wake
// where there's room; the same world at 1, 4 and 20 threads, under reversed
// dispatch, and through a snapshot taken with programs drifting and resting.
const char* const kDormantExtra = "life.frozen_ticks = 40\nlife.dormancy = true\nmem.upkeep = 6\nworld.disk = 1024\n";

void test_dormant_world() {
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append(kDormantExtra);
    u64 hashes[4] = {};
    const u32 threads[4] = {1, 4, 20, 4};
    Array<u32> order;
    SoupTally sum = {};
    u64 most_drifting = 0, snap_tick = 0;
    ByteWriter snap;
    for (u32 v = 0; v < 4; ++v) {
        Run run;
        CHECK(make_run(run, cfg.c_str(), threads[v]));
        if (v == 3) {
            const u32 n = run.world().node_count();
            order.resize(n);
            for (u32 i = 0; i < n; ++i)
                order[i] = n - 1 - i;
            run.set_dispatch_order(order.data());
        }
        for (u64 t = 1; t <= 400; ++t) {
            run.run_until(t);
            if (v == 0) {
                const SoupTally& s = run.world().last_soup_tick();
                sum.dormant_started += s.dormant_started;
                sum.dormant_woke += s.dormant_woke;
                sum.dormant_expired += s.dormant_expired;
                sum.dormant_hops += s.dormant_hops;
                const u64 dr = run.world().drifting();
                if (dr > most_drifting && snap.size() == 0 && t > 50) { // a snapshot with programs on the move
                    most_drifting = dr;
                    run.snapshot(snap);
                    snap_tick = t;
                }
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
    CHECK(sum.dormant_started > 0 && sum.dormant_woke > 0 && sum.dormant_hops > 0);
    CHECK(snap.size() > 0);
    if (snap.size() > 0) {
        Run b;
        TextBuf err;
        CHECK(b.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        b.run_until(400);
        CHECK_EQ_U64(b.world().hash(), hashes[0]);
    }
    printf("  dormant world: 400 ticks, %llu went dormant, %llu woke, %llu expired, %llu hops; snapshot at tick %llu "
           "with %llu drifting resumed exactly; hash %016llx\n",
           static_cast<unsigned long long>(sum.dormant_started), static_cast<unsigned long long>(sum.dormant_woke),
           static_cast<unsigned long long>(sum.dormant_expired), static_cast<unsigned long long>(sum.dormant_hops),
           static_cast<unsigned long long>(snap_tick), static_cast<unsigned long long>(most_drifting),
           static_cast<unsigned long long>(hashes[0]));
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
    eat.append("ENGULF NOP0"); // 60 codons: a third is 20 (docs/bites-plan.md B2)
    for (int i = 0; i < 58; ++i)
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
        CHECK(p.ambient == 0.05f && p.endurance == 0 && !p.map); // (no lifecycle without a map)
        CHECK(p.mem_decay > 0 && p.rot_rate > 0 && p.rot_rate < p.mem_decay * 0.011 && p.rot_rate > p.mem_decay * 0.009);
        CHECK(p.disk_decay == 1e-6 && p.disk_rot == 1e-8);
        CHECK(p.vent_spacing == 4096 && p.vent_rate == 64 && p.vent_radius == 256);
        CHECK(p.bites && p.engulf_cpt == 8 && p.threads_end && p.frozen_ticks == 200);
        CHECK(p.dormancy && p.dormant_mem_loss == 1 && p.dormant_disk_loss == 1 && p.dormant_dwell == 15);
        // a varied world (docs/world-plan.md W5; the world sets each node's substrate speeds)
        CHECK(p.spark_activity == 2.0 && p.p_speed == 1.0f && p.gpu_speed == 1.0f && p.disk_speed == 1.0f);
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
    // ... and resumed from a snapshot exactly, on every default.
    const u64 snap_tick = 50;
    {
        Run run;
        CHECK(make_run(run, w, 4));
        run.run_until(snap_tick);
        ByteWriter snap;
        run.snapshot(snap);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(snap.data(), snap.size(), 4, &err));
        run.run_until(120);
        resumed.run_until(120);
        CHECK_EQ_U64(resumed.world().hash(), run.world().hash());
    }
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    printf("  defaults world: 100 ticks, hash %016llx; snapshot at tick %llu resumed exactly\n",
           static_cast<unsigned long long>(h[0]), static_cast<unsigned long long>(snap_tick));
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
    test_dormant();
    test_dormant_lineage();
    test_dormant_bitten();
    test_dormant_world();
    test_eco_world();
    test_defaults();
}
