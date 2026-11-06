// Phase 1b instruction-set controls (design.md 6.6): the plain-64 table and
// its 16 new instructions (D7), the doubled-invasion table (D8), and the
// selector maps behind the shuffled-selector control (D6).
#include <cstring>

#include "check.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

struct Fix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;

    bool init(const char* prog, const char* extra = "", u32 at = 100, u64 seed = 5) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, seed, 0);
        b = plant(s, at, prog, 500);
        if (b == kNone)
            return false;
        c = s.body(b).cpus;
        s.begin_tick(0);
        return true;
    }
    Cpu& cpu() { return s.cpu_mut(c); }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i)
            s.run_cpu(c, 1);
    }
    void regs(u32 ax, u32 bx, u32 cx) {
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
    }
};

const char* const kPlain = "isa.table = nova_v1_plain\n";

u32 neg(u32 v) {
    return 0u - v;
}

void table_layout() {
    CodonTable v1, plain, inv2;
    CHECK(builtin_table("nova_v1", &v1));
    CHECK(builtin_table("nova_v1_plain", &plain));
    CHECK(builtin_table("nova_v1_inv2", &inv2));
    const Op partners[16] = {Op::nopx, Op::jmpf, Op::jmpb, Op::ifeq, Op::ifgt,  Op::mod,
                             Op::min_, Op::max_, Op::dup,  Op::drop, Op::inca,  Op::deca,
                             Op::incb, Op::decb, Op::zeroa, Op::zerob};
    bool plain_ok = true, inv2_ok = true;
    for (u32 c = 0; c < 64; ++c) {
        Op want = c < 32 && (c & 1) ? partners[c >> 1] : v1.op[c];
        plain_ok = plain_ok && plain.op[c] == want;
        Op want2 = c == 58 ? Op::engulf : (c == 59 ? Op::inject : v1.op[c]);
        inv2_ok = inv2_ok && inv2.op[c] == want2;
    }
    CHECK(plain_ok && inv2_ok);
    // Plain-64: 64 distinct instructions, every one on exactly one codon.
    bool distinct = true;
    for (u32 a = 0; a < 64; ++a)
        for (u32 b = a + 1; b < 64; ++b)
            distinct = distinct && plain.op[a] != plain.op[b];
    CHECK(distinct);
    // The doubled pairs differ in bit 1.
    CHECK((56 ^ 58) == 2 && (57 ^ 59) == 2);
    CHECK(Str(plain.name).eq("nova_v1_plain") && Str(inv2.name).eq("nova_v1_inv2"));
    // Names round-trip and the new ones parse.
    Op op;
    CHECK(op_from_name("ZEROB", &op) && op == Op::zerob && Str(op_name(Op::nopx)).eq("NOPX"));
    SoupParams p;
    CHECK(make_params(&p, "isa.table = nova_v1_inv2\n", 1024));
    CHECK(!make_params(&p, "isa.table = nova_v2\n", 1024));
}

void plain_registers() {
    struct Case {
        const char* op;
        u32 ax, bx, cx;
        u32 want_ax, want_bx, want_cx;
        bool err;
    };
    const Case cases[] = {
        {"INCA", 5, 6, 7, 6, 6, 7, false},       {"DECA", 0, 6, 7, neg(1), 6, 7, false},
        {"INCB", 5, 6, 7, 5, 7, 7, false},       {"DECB", 5, 6, 7, 5, 5, 7, false},
        {"ZEROA", 5, 6, 7, 0, 6, 7, false},      {"ZEROB", 5, 6, 7, 5, 0, 7, false},
        {"MOD", 7, 3, 0, 7, 3, 1, false},        {"MOD", neg(7), 3, 0, neg(7), 3, neg(1), false},
        {"MOD", 7, 0, 9, 7, 0, 9, true},         {"MIN", neg(2), 3, 0, neg(2), 3, neg(2), false},
        {"MAX", neg(2), 3, 0, neg(2), 3, 3, false}, {"NOPX", 1, 2, 3, 1, 2, 3, false},
    };
    for (const Case& k : cases) {
        Fix f;
        CHECK(f.init(k.op, kPlain));
        f.regs(k.ax, k.bx, k.cx);
        f.run(1);
        const Cpu& c = f.cpu();
        bool good = c.ax == k.want_ax && c.bx == k.want_bx && c.cx == k.want_cx &&
                    c.err == (k.err ? 1 : 0);
        if (!good)
            fprintf(stderr, "  tables: %s gave ax=%u bx=%u cx=%u err=%u\n", k.op, c.ax, c.bx, c.cx,
                    c.err);
        CHECK(good);
    }
}

void plain_flow() {
    {
        Fix f; // IFEQ skips one instruction unless AX == BX
        CHECK(f.init("IFEQ INC INC", kPlain));
        f.regs(1, 2, 0);
        f.run(2);
        CHECK(f.cpu().cx == 1 && f.cpu().ip == 100);
    }
    {
        Fix f;
        CHECK(f.init("IFEQ INC INC", kPlain));
        f.regs(2, 2, 0);
        f.run(3);
        CHECK(f.cpu().cx == 2);
    }
    {
        Fix f; // IFGT is signed: 0 > -1
        CHECK(f.init("IFGT INC INC", kPlain));
        f.regs(0, neg(1), 0);
        f.run(3);
        CHECK(f.cpu().cx == 2);
    }
    {
        Fix f; // DUP pushes the top again; DROP discards; both fail when they can't
        CHECK(f.init("PUSH DUP DROP DROP DROP DUP", kPlain));
        f.regs(0, 0, 9);
        f.run(2);
        CHECK(f.cpu().sp == 2 && f.cpu().stack[1] == 9 && f.cpu().err == 0);
        f.run(2);
        CHECK(f.cpu().sp == 0 && f.cpu().err == 0);
        f.run(1);
        CHECK(f.cpu().err == 1); // DROP on an empty stack
        f.run(1);
        CHECK(f.cpu().err == 1); // DUP on an empty stack
    }
    {
        // JMPF searches forward only: the backward complement is nearer, and
        // JMP would take it (codons 100-107).
        Fix f;
        CHECK(f.init("NOP1 INC JMPF NOP0 INC INC NOP1 INC", kPlain));
        f.cpu().ip = 102;
        f.run(1);
        CHECK(f.cpu().err == 0 && f.cpu().ip == 107);
        Fix g;
        CHECK(g.init("NOP1 INC JMP NOP0 INC INC NOP1 INC", kPlain));
        g.cpu().ip = 102;
        g.run(1);
        CHECK(g.cpu().ip == 101);
    }
    {
        // JMPB searches backward only: at equal distance JMP goes forward.
        Fix f;
        CHECK(f.init("NOP1 INC JMPB NOP0 INC NOP1 INC", kPlain));
        f.cpu().ip = 102;
        f.run(1);
        CHECK(f.cpu().err == 0 && f.cpu().ip == 101);
        Fix g;
        CHECK(g.init("NOP1 INC JMP NOP0 INC NOP1 INC", kPlain));
        g.cpu().ip = 102;
        g.run(1);
        CHECK(g.cpu().ip == 106);
    }
    {
        Fix f; // NOPX ends a template: JMP NOP0 NOPX NOP1 searches for "1", not "10"
        CHECK(f.init("JMP NOP0 NOPX NOP1 INC NOP1 NOP0 INC", kPlain));
        f.run(1);
        CHECK(f.cpu().err == 0 && f.cpu().ip == 104); // after the first NOP1
    }
}

void inv2_contact() {
    // Codons 58/59 now attack: a blind ENGULF on codon 58 eats a surfaceless
    // neighbour, where nova_v1 would fail it as EMIT.
    for (int doubled = 0; doubled < 2; ++doubled) {
        SoupParams p;
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\n");
        if (doubled)
            cfg.append("isa.table = nova_v1_inv2\n");
        CHECK(make_params(&p, cfg.c_str(), 1024));
        Soup s;
        s.init(&p, 5, 0);
        s.mem_mut()[100] = 58;
        s.mem_mut()[101] = 8; // INC
        u32 eater = s.create_body(100, 2, 100);
        u32 prey = plant(s, 150, "INC INC", 100);
        CHECK(eater != kNone && prey != kNone);
        s.begin_tick(0);
        s.run_cpu(s.body(eater).cpus, 1);
        CHECK((s.tally().engulfs == 1) == (doubled == 1));
    }
}

// The expected value of SENSE reading r for a lone 1-codon body.
u32 expect_reading(const Soup& s, u32 b, u32 r) {
    const Body& B = s.body(b);
    switch (r) {
    case 0:
    case 1:
        return 1; // block size, memory owned
    case 2:
        return B.units;
    case 3:
    case 4:
        return 0; // heat off
    case 5:
        return s.free_units();
    case 6:
        return s.waste();
    default:
        return B.ncpu;
    }
}

void selectors() {
    // Nearest: the identity everywhere, as in Phase 1a.
    SoupParams p;
    CHECK(make_params(&p, "", 1024));
    bool identity = !p.sel.shuffled;
    for (u32 i = 0; i < 8; ++i)
        identity = identity && p.sel.sense[i] == i;
    for (u32 i = 0; i < 4; ++i)
        identity = identity && p.sel.res[i] == i && p.sel.divide[i] == i;
    identity = identity && p.sel.head[0] == 0 && p.sel.head[1] == 1;
    CHECK(identity);
    TextBuf d;
    describe_selectors(p.sel, d);
    CHECK(d.view().eq("nearest"));

    // Shuffled: a permutation per map, self never at 0, a pure function of
    // the seed, and different between seeds.
    auto perm_ok = [](const u8* m, u32 n) {
        u32 seen = 0;
        for (u32 i = 0; i < n; ++i)
            seen |= 1u << m[i];
        return seen == (1u << n) - 1 && m[0] != 0;
    };
    u32 distinct_sense0 = 0, seen_sense0 = 0;
    bool all_ok = true;
    for (u64 seed = 1; seed <= 40; ++seed) {
        TextBuf cfg;
        cfg.append("isa.selectors = shuffled\nseed = ");
        cfg.append_u64(seed);
        cfg.append_char('\n');
        SoupParams a, b;
        CHECK(make_params(&a, cfg.c_str(), 1024));
        CHECK(make_params(&b, cfg.c_str(), 1024));
        all_ok = all_ok && a.sel.shuffled && perm_ok(a.sel.sense, 8) && perm_ok(a.sel.res, 4) &&
                 perm_ok(a.sel.head, 2) && perm_ok(a.sel.divide, 4) &&
                 memcmp(&a.sel, &b.sel, sizeof a.sel) == 0;
        if (!(seen_sense0 & (1u << a.sel.sense[0]))) {
            seen_sense0 |= 1u << a.sel.sense[0];
            ++distinct_sense0;
        }
    }
    CHECK(all_ok);
    CHECK(distinct_sense0 >= 5); // SENSE 0 lands on many different readings
    // isa.selector_seed overrides the world seed.
    SoupParams x, y;
    CHECK(make_params(&x, "isa.selectors = shuffled\nseed = 1\nisa.selector_seed = 99\n", 1024));
    CHECK(make_params(&y, "isa.selectors = shuffled\nseed = 2\nisa.selector_seed = 99\n", 1024));
    CHECK(memcmp(&x.sel, &y.sel, sizeof x.sel) == 0);
    CHECK(!make_params(&x, "isa.selectors = random\n", 1024));

    // Behaviour follows the map: SENSE with BX = v gives reading sense[v].
    const char* shuf = "isa.selectors = shuffled\nseed = 3\n";
    bool readings = true;
    for (u32 v = 0; v < 8; ++v) {
        Fix f;
        CHECK(f.init("SENSE", shuf));
        f.s.cpu_mut(f.c).bx = v;
        f.run(1);
        u32 r = f.p.sel.sense[v];
        readings = readings && f.cpu().cx == expect_reading(f.s, f.b, r);
    }
    CHECK(readings);

    // ALLOC and DIVIDE: BX = 0 no longer means memory / here; their mapped
    // slots do.
    {
        Fix f;
        CHECK(f.init("ALLOC DIVIDE DIVIDE", shuf));
        u32 mem_slot = 0, here_slot = 0;
        for (u32 i = 0; i < 4; ++i) {
            if (f.p.sel.res[i] == 0)
                mem_slot = i;
            if (f.p.sel.divide[i] == 0)
                here_slot = i;
        }
        CHECK(mem_slot != 0 && here_slot != 0);
        f.regs(0, mem_slot, 8);
        f.run(1); // ALLOC 8 codons
        CHECK(f.cpu().err == 0 && f.s.body(f.b).latest != kNone);
        f.regs(0, 0, 32);
        f.run(1); // DIVIDE with BX = 0: a missing neighbour on one node
        CHECK(f.cpu().err == 1 && f.s.tally().births == 0);
        f.regs(0, here_slot, 32);
        f.run(1); // DIVIDE here
        CHECK(f.cpu().err == 0 && f.s.tally().births == 1);
    }
    {
        Fix f; // SETH through the head map
        CHECK(f.init("SETH", shuf));
        f.regs(500, 0, 0);
        f.run(1);
        CHECK(f.p.sel.head[0] == 1 && f.cpu().wh == 500 && f.cpu().rh == 100);
    }
}

} // namespace

void suite_tables() {
    table_layout();
    plain_registers();
    plain_flow();
    inv2_contact();
    selectors();
}
