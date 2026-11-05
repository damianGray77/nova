// ENGULF and INJECT (design.md 6.1, 6.4 #3, #6-9, 6.5): the graded surface
// contest, what an engulf transfers, engulf rules 1-3, INJECT's overwrite and
// the costs.
#include <cmath>

#include "check.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

struct Fix {
    SoupParams p;
    Soup s;
    bool init(const char* extra = "", bool heat = false) {
        TextBuf cfg;
        cfg.append(heat ? "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n" : kQuiet);
        cfg.append("world.millicores = 2000\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 23, 0);
        s.begin_tick(0);
        return true;
    }
    u32 cpu_of(u32 b) const { return s.body(b).cpus; }
    Cpu& cpu(u32 b) { return s.cpu_mut(s.body(b).cpus); }
};

// A perfect match always wins: the prey's blocks, CPU and millicores pass to
// the eater, and the prey keeps running inside it (rule 1).
void engulf_perfect() {
    Fix f;
    CHECK(f.init());
    u32 eater = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 INC", 500);
    u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 INC", 300); // surface 110
    CHECK(eater != kNone && prey != kNone);
    i64 cost = f.s.run_cpu(f.cpu_of(eater), 1);
    CHECK(f.s.tally().engulfs == 1 && f.cpu(eater).err == 0);
    CHECK(cost == 1 + 8 + 4); // the instruction + contact.cost_base + 1 per codon moved
    CHECK(f.cpu(eater).ip == 104); // past its template
    const Body& e = f.s.body(eater);
    CHECK(e.mem == 9 && e.units == 800 && e.ncpu == 2);
    CHECK(f.s.owner_body(200) == eater && !f.s.body(prey).alive);
    f.s.death_sweep();
    CHECK(f.s.body_count() == 1 && soup_ok(f.s));

    // Next tick both CPUs run, splitting the body's 800 instructions equally.
    u32 prey_cpu = f.s.cpu(f.cpu_of(eater)).next;
    CHECK(prey_cpu != kNone && f.s.cpu(prey_cpu).body == eater);
    f.s.tick(1);
    CHECK(f.s.cpu(prey_cpu).executed == 400);
    CHECK(soup_ok(f.s));
}

// Rule 2: one body, one set of write rights -- both ways. The host can jam the
// prey (rule-1 CPUs halt after K errors, the body lives on); the prey can
// overwrite the host.
void shared_rights() {
    Fix f;
    CHECK(f.init());
    u32 host = plant(f.s, 100, "ENGULF NOP0 NOP0 NOP1 WRITE", 500);
    u32 prey = plant(f.s, 200, "NOP1 NOP1 NOP0 WRITE", 300);
    CHECK(host != kNone && prey != kNone);
    f.s.run_cpu(f.cpu_of(host), 1);
    CHECK(f.s.tally().engulfs == 1);
    u32 prey_cpu = f.s.cpu(f.cpu_of(host)).next;

    // The prey writes into the host's original code.
    Cpu& pc = f.s.cpu_mut(prey_cpu);
    pc.ip = 203;
    pc.wh = 100;
    pc.cx = 7;
    f.s.run_cpu(prey_cpu, 1);
    CHECK(f.s.cpu(prey_cpu).err == 0 && f.s.mem()[100] == 7);

    // The host writes JMP-with-no-template over the prey's code.
    int jmp = codon_of(f.p.table, Op::jmp);
    for (u32 a = 200; a < 204; ++a) {
        Cpu& hc = f.cpu(host);
        hc.ip = 104;
        hc.wh = a;
        hc.cx = static_cast<u32>(jmp);
        f.s.run_cpu(f.cpu_of(host), 1);
        CHECK(f.cpu(host).err == 0);
    }
    f.s.cpu_mut(prey_cpu).ip = 200;
    for (u32 i = 0; i < 8; ++i)
        f.s.run_cpu(prey_cpu, 1);
    CHECK(f.s.cpu(prey_cpu).halted);
    f.s.death_sweep();
    CHECK(f.s.body_count() == 1 && f.s.body(host).alive && soup_ok(f.s));
}

// Graded: template 001 against surface 111 matches 2 of 3, so it wins 2/3 of
// the time. INJECT keeps the victim alive, so it can be retried.
void graded() {
    Fix f;
    CHECK(f.init());
    u32 a = plant(f.s, 100, "INJECT NOP0 NOP0 NOP1 INC", 500);
    u32 v = plant(f.s, 200, "NOP1 NOP1 NOP1 INC", 300);
    CHECK(a != kNone && v != kNone);
    const u32 trials = 3000;
    u64 cost = 0;
    for (u32 i = 0; i < trials; ++i) {
        Cpu& c = f.cpu(a);
        c.ip = 100;
        c.ax = 3; // land on the victim's INC, leaving its surface alone
        c.cx = 1;
        c.rh = 104;
        c.errs = 0; // a losing streak must not reach the K = 8 halt
        cost += static_cast<u64>(f.s.run_cpu(f.cpu_of(a), 1));
    }
    u64 wins = f.s.tally().injects;
    CHECK(f.s.tally().inject_tries == trials);
    double rate = static_cast<double>(wins) / trials;
    CHECK(fabs(rate - 2.0 / 3.0) < 5 * sqrt((2.0 / 9.0) / trials));
    CHECK(cost == trials * 9 + wins * 1); // every attempt pays; wins pay per codon
}

// 0/0 counts as a perfect match: a blind attack takes a surfaceless body for
// sure, never a body with a surface; an aimed attack never takes a surfaceless
// one. Out of contact range there is no target at all.
void blind_and_aimed() {
    Fix f;
    CHECK(f.init());
    u32 a = plant(f.s, 100, "ENGULF INC", 500);
    u32 shielded = plant(f.s, 150, "NOP1 INC", 100); // nearer, but has a surface
    u32 bare = plant(f.s, 200, "INC INC", 100);
    CHECK(a != kNone && shielded != kNone && bare != kNone);
    f.s.run_cpu(f.cpu_of(a), 1);
    CHECK(f.s.tally().engulfs == 1 && !f.s.body(bare).alive && f.s.body(shielded).alive);
    for (u32 i = 0; i < 50; ++i) {
        f.cpu(a).ip = 100;
        f.cpu(a).errs = 0;
        f.s.run_cpu(f.cpu_of(a), 1);
    }
    CHECK(f.s.tally().engulf_tries == 51);
    CHECK(f.s.tally().engulfs == 1 && f.s.body(shielded).alive);

    Fix g;
    CHECK(g.init());
    u32 b = plant(g.s, 100, "ENGULF NOP0 INC", 500);
    CHECK(plant(g.s, 200, "INC INC", 100) != kNone);
    for (u32 i = 0; i < 50; ++i) {
        g.cpu(b).ip = 100;
        g.cpu(b).errs = 0;
        g.s.run_cpu(g.cpu_of(b), 1);
    }
    CHECK(g.s.tally().engulfs == 0 && g.s.tally().engulf_tries == 50);

    Fix h;
    CHECK(h.init("contact.range = 50\n"));
    u32 c = plant(h.s, 100, "ENGULF INC", 500);
    CHECK(plant(h.s, 300, "INC INC", 100) != kNone);
    CHECK(h.s.run_cpu(h.cpu_of(c), 1) == 9); // a failed attempt still costs
    CHECK(h.cpu(c).err == 1 && h.s.tally().engulfs == 0);
}

// INJECT overwrites, never inserts: the victim keeps its size and runs on.
void inject_overwrites() {
    Fix f;
    CHECK(f.init());
    u32 a = plant(f.s, 100, "INJECT NOP0 NOP0 NOP1 DEC DEC DEC", 500);
    u32 v = plant(f.s, 300, "NOP1 NOP1 NOP0 INC INC INC", 300);
    CHECK(a != kNone && v != kNone);
    Cpu& c = f.cpu(a);
    c.rh = 104;
    c.ax = 3;
    c.cx = 3;
    CHECK(f.s.run_cpu(f.cpu_of(a), 1) == 1 + 8 + 3);
    int dec = codon_of(f.p.table, Op::dec);
    for (u32 i = 303; i < 306; ++i)
        CHECK(f.s.mem()[i] == dec);
    CHECK(f.s.body(v).mem == 6 && f.s.body(v).alive && f.s.block(f.s.body(v).blocks).len == 6);
    CHECK(f.s.body(a).mem == 7);
    // The victim runs the injected code.
    f.s.cpu_mut(f.cpu_of(v)).ip = 303;
    f.s.run_cpu(f.cpu_of(v), 1);
    CHECK(f.s.cpu(f.cpu_of(v)).cx == 0u - 1u);

    // Landing wraps within the victim's block; zero means nothing.
    f.cpu(a).ip = 100;
    f.cpu(a).rh = 104;
    f.cpu(a).ax = 7; // 7 mod 6 = codon 1
    f.cpu(a).cx = 1;
    f.s.run_cpu(f.cpu_of(a), 1);
    CHECK(f.s.mem()[301] == dec);
    f.cpu(a).ip = 100;
    f.cpu(a).cx = 0;
    CHECK(f.s.run_cpu(f.cpu_of(a), 1) == 1 && f.cpu(a).err == 1);
    CHECK(soup_ok(f.s));
}

// An engulf moves the prey's millicores with their heat.
void heat_moves_with_units() {
    Fix f;
    CHECK(f.init("", true));
    u32 eater = plant(f.s, 100, "ENGULF NOP0 INC", 500);
    u32 prey = plant(f.s, 200, "NOP1 INC", 300);
    for (u32 u = 0; u < f.p.millicores; ++u)
        if (f.s.unit_owner(u) == prey)
            f.s.set_unit_heat(u, 50);
    f.s.run_cpu(f.cpu_of(eater), 1);
    u32 hot = 0;
    for (u32 u = 0; u < f.p.millicores; ++u)
        hot += f.s.unit_owner(u) == eater && f.s.unit_heat(u) == 50;
    CHECK(f.s.tally().engulfs == 1 && hot == 300);
}

} // namespace

void suite_contact() {
    engulf_perfect();
    shared_rights();
    graded();
    blind_and_aimed();
    inject_overwrites();
    heat_moves_with_units();
}
