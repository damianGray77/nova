// Interpreter semantics for codon table v1 (design.md 6.2, 6.5), one
// instruction family at a time, plus the private-code bounds.
#include "check.h"
#include "soup_helpers.h"

using namespace nova;

namespace {

// A fresh quiet soup (1024 codons, 1000 millicores) with one program planted
// at `at` as a body holding 500 of them.
struct Fix {
    SoupParams p;
    Soup s;
    u32 b = kNone, c = kNone;

    bool init(const char* prog, const char* extra = "", u32 at = 100) {
        TextBuf cfg;
        cfg.append(kQuiet);
        cfg.append("world.millicores = 1000\n");
        cfg.append(extra);
        if (!make_params(&p, cfg.c_str(), 1024))
            return false;
        s.init(&p, 5, 0);
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
    // Executes the first instruction with the given registers.
    void regs(u32 ax, u32 bx, u32 cx) {
        cpu().ax = ax;
        cpu().bx = bx;
        cpu().cx = cx;
    }
};

u32 neg(u32 v) {
    return 0u - v;
}

void registers() {
    {
        Fix f;
        CHECK(f.init("ROT"));
        f.regs(1, 2, 3);
        f.run(1);
        CHECK(f.cpu().ax == 3 && f.cpu().bx == 1 && f.cpu().cx == 2); // AX->BX->CX->AX
    }
    {
        Fix f;
        CHECK(f.init("ROTB"));
        f.regs(1, 2, 3);
        f.run(1);
        CHECK(f.cpu().ax == 2 && f.cpu().bx == 3 && f.cpu().cx == 1);
    }
    {
        Fix f;
        CHECK(f.init("SWAP"));
        f.regs(1, 2, 3);
        f.run(1);
        CHECK(f.cpu().ax == 2 && f.cpu().bx == 1 && f.cpu().cx == 3);
    }
    struct Case {
        const char* op;
        u32 ax, bx, cx, want;
        bool err;
    };
    const Case cases[] = {
        {"INC", 0, 0, 7, 8, false},          {"DEC", 0, 0, 0, neg(1), false},
        {"ZERO", 0, 0, 9, 0, false},         {"ADD", 5, 7, 0, 12, false},
        {"SUB", 5, 7, 0, neg(2), false},     {"MUL", neg(3), 7, 0, neg(21), false},
        {"DIV", neg(7), 2, 0, neg(3), false}, // truncates toward zero, signed
        {"DIV", 7, 0, 99, 99, true},          // divide by zero fails, CX untouched
        {"AND", 12, 10, 0, 8, false},        {"OR", 12, 10, 0, 14, false},
        {"XOR", 12, 10, 0, 6, false},        {"NOT", 0, 0, 0, 0xFFFFFFFFu, false},
        {"SHL", 0, 0, 3, 6, false},          {"SHR", 0, 0, 7, 3, false},
        {"NEG", 0, 0, 5, neg(5), false},
    };
    for (const Case& k : cases) {
        Fix f;
        CHECK(f.init(k.op));
        f.regs(k.ax, k.bx, k.cx);
        f.run(1);
        bool good = f.cpu().cx == k.want && f.cpu().err == (k.err ? 1 : 0);
        if (!good)
            fprintf(stderr, "  registers: %s gave cx=%u err=%u\n", k.op, f.cpu().cx, f.cpu().err);
        CHECK(good);
    }
}

void conditionals() {
    {
        Fix f; // IFZ skips exactly one instruction when CX != 0
        CHECK(f.init("IFZ INC INC"));
        f.regs(0, 0, 5);
        f.run(2); // IFZ, then the second INC
        CHECK(f.cpu().cx == 6 && f.cpu().ip == 100);
    }
    {
        Fix f;
        CHECK(f.init("IFZ INC INC"));
        f.run(3);
        CHECK(f.cpu().cx == 2);
    }
    {
        Fix f; // IFLT is signed: -1 < 0
        CHECK(f.init("IFLT INC NOP0"));
        f.regs(neg(1), 0, 0);
        f.run(2);
        CHECK(f.cpu().cx == 1);
    }
    {
        Fix f; // IFNE skips when equal
        CHECK(f.init("IFNE INC NOP0"));
        f.regs(4, 4, 0);
        f.run(2);
        CHECK(f.cpu().cx == 0 && f.cpu().ip == 100);
    }
    {
        Fix f; // IFERR tests the previous instruction's flag
        CHECK(f.init("POP IFERR INC NOP0"));
        f.run(3); // POP fails (empty), IFERR passes, INC runs
        CHECK(f.cpu().cx == 1);
    }
}

void stack() {
    Fix f;
    CHECK(f.init("PUSH"));
    for (u32 i = 0; i < 8; ++i) {
        f.cpu().cx = i;
        f.run(1);
        CHECK(f.cpu().err == 0);
    }
    f.run(1); // the 9th push overflows the 8-deep stack
    CHECK(f.cpu().err == 1 && f.cpu().sp == 8);

    Fix g;
    CHECK(g.init("PUSH INC POP"));
    g.cpu().cx = 41;
    g.run(3);
    CHECK(g.cpu().cx == 41 && g.cpu().sp == 0);
    g.run(2); // PUSH 41, INC
    g.run(1); // POP
    g.run(1); // wraps to PUSH
    CHECK(g.cpu().sp == 1);
}

void templates() {
    {
        Fix f; // JMP lands after the complementary template
        CHECK(f.init("JMP NOP0 NOP0 INC NOP1 NOP1 DEC"));
        f.run(2);
        CHECK(f.cpu().cx == neg(1) && f.cpu().err == 0);
    }
    {
        Fix f; // no template: nothing to aim at, so it fails and moves on
        CHECK(f.init("JMP INC"));
        f.run(1);
        CHECK(f.cpu().err == 1 && f.cpu().ip == 101);
    }
    {
        Fix f; // no complement: fails and skips its template
        CHECK(f.init("JMP NOP0 NOP0 INC"));
        f.run(1);
        CHECK(f.cpu().err == 1 && f.cpu().ip == 103);
    }
    {
        Fix f; // CALL pushes the address after its template; RET returns there
        CHECK(f.init("CALL NOP0 INC INC NOP1 RET"));
        f.run(3); // CALL, RET, INC
        CHECK(f.cpu().cx == 1 && f.cpu().sp == 0 && f.cpu().ip == 103);
    }
    {
        Fix f; // FIND: AX = address after the match
        CHECK(f.init("FIND NOP1 NOP0 ZERO NOP0 NOP1 INC"));
        f.run(1);
        CHECK(f.cpu().ax == 106 && f.cpu().ip == 103);
    }
    {
        Fix f; // DJNZ loops until CX hits zero, then skips its template
        CHECK(f.init("NOP1 DJNZ NOP0 INC"));
        f.cpu().cx = 3;
        f.run(5); // NOP1, DJNZ x3, INC
        CHECK(f.cpu().cx == 1);
    }
    {
        Fix f; // JZ jumps only on zero
        CHECK(f.init("NOP1 JZ NOP0 INC"));
        f.cpu().cx = 2;
        f.run(3); // NOP1, JZ (falls through), INC
        CHECK(f.cpu().cx == 3);
        f.cpu().cx = 0;
        f.cpu().ip = 101;
        f.run(1);
        CHECK(f.cpu().ip == 101); // jumped back to just after NOP1
    }
    {
        Fix f; // searches stay in the searcher's own body (private code)
        CHECK(f.init("JMP NOP0 NOP0 INC"));
        CHECK(plant(f.s, 120, "NOP1 NOP1 DEC", 100) != kNone);
        f.run(1);
        CHECK(f.cpu().err == 1);
    }
}

void bounds_and_wrap() {
    {
        Fix f; // IP wraps at the end of its block
        CHECK(f.init("INC INC"));
        f.run(5);
        CHECK(f.cpu().cx == 5 && f.cpu().ip == 101);
    }
    {
        Fix f; // heads wrap at the end of their block too (design.md 6.5)
        CHECK(f.init("READ READ READ"));
        f.run(3);
        CHECK(f.cpu().rh == 100);
    }
    {
        Fix f; // another body's code can be neither read nor executed
        CHECK(f.init("SETH COPY NOP0"));
        u32 other = plant(f.s, 300, "INC INC", 100);
        CHECK(other != kNone);
        f.regs(300, 0, 0); // read head -> the other body
        f.run(2);
        CHECK(f.cpu().err == 1 && f.cpu().rh == 300);
        f.cpu().ip = 300; // jump into it by force
        f.run(1);
        CHECK(f.cpu().err == 1 && f.cpu().cx == 0);
    }
    {
        Fix f; // waste is readable but not writable
        CHECK(f.init("SETH READ NOP0"));
        f.regs(600, 0, 0);
        f.run(2);
        CHECK(f.cpu().err == 0 && f.cpu().cx == f.s.mem()[600] && f.cpu().rh == 601);

        Fix g;
        CHECK(g.init("SETH WRITE NOP0"));
        g.regs(600, 1, 5); // write head -> waste
        g.run(2);
        CHECK(g.cpu().err == 1);
        g.cpu().wh = 102; // own body
        g.cpu().ip = 101;
        g.run(1);
        CHECK(g.cpu().err == 0 && g.s.mem()[102] == 5);
    }
    {
        Fix f; // K = 8 consecutive errors halt the CPU
        CHECK(f.init("JMP"));
        f.run(7);
        CHECK(!f.cpu().halted && f.cpu().errs == 7);
        f.run(1);
        CHECK(f.cpu().halted && f.s.tally().halts == 1);
        CHECK(f.s.run_cpu(f.c, 10) == 0); // a halted CPU does nothing
    }
    {
        Fix f; // a success resets the count
        CHECK(f.init("JMP JMP JMP INC"));
        f.run(10); // 3 fails, INC, 3 fails, INC, 2 fails
        CHECK(!f.cpu().halted && f.cpu().errs == 2);
    }
}

void world_verbs() {
    {
        Fix f; // SENSE readings, nearest to self first
        CHECK(f.init("SENSE"));
        f.regs(0, 0, 0);
        f.run(1);
        CHECK(f.cpu().cx == 1); // my size
        f.cpu().bx = 2;
        f.run(1);
        CHECK(f.cpu().cx == 500); // my millicores
        f.cpu().bx = 5;
        f.run(1);
        CHECK(f.cpu().cx == 500); // free millicores in the pool
        f.cpu().bx = 6;
        f.run(1);
        CHECK(f.cpu().cx == 1023); // waste
        f.cpu().bx = 8;
        f.run(1);
        CHECK(f.cpu().err == 1);
    }
    {
        Fix f; // ALLOC memory: nearest free run, write head to its start
        CHECK(f.init("ALLOC NOP0"));
        f.regs(0, 0, 10);
        f.run(1);
        const Body& b = f.s.body(f.b);
        CHECK(f.cpu().err == 0 && f.cpu().wh == 102 && b.mem == 12 && b.latest != kNone);
        CHECK(f.s.owner_body(111) == f.b && f.s.owner_body(112) == kNone);
        CHECK(soup_ok(f.s));
    }
    {
        Fix f; // zero means nothing; CPU claims are capped by the pool; GPU is off
        CHECK(f.init("ALLOC"));
        f.run(1);
        CHECK(f.cpu().err == 0 && f.s.body(f.b).mem == 1);
        f.regs(0, 1, 900); // CPU: only 500 left in the pool
        f.run(1);
        CHECK(f.cpu().err == 0 && f.s.body(f.b).units == 1000 && f.s.free_units() == 0);
        f.run(1);
        CHECK(f.cpu().err == 1); // pool empty
        f.regs(0, 2, 5);
        f.run(1);
        CHECK(f.cpu().err == 1);
        CHECK(soup_ok(f.s));
    }
    {
        Fix f; // DIVIDE: CX/64 of the CPU, the latest block becomes her body
        CHECK(f.init("ALLOC DIVIDE"));
        f.regs(0, 0, 4);
        f.run(1);
        f.cpu().cx = 16; // a quarter
        f.run(1);
        CHECK(f.cpu().err == 0 && f.s.tally().births == 1);
        CHECK(f.s.body(f.b).units == 375 && f.s.body(f.b).mem == 2 && f.s.body_count() == 2);
        u32 d = f.s.body_at(1);
        CHECK(f.s.body(d).units == 125 && f.s.body(d).mem == 4 && f.s.body(d).ncpu == 1);
        CHECK(f.s.cpu(f.s.body(d).cpus).ip == 102);
        f.cpu().ip = 101;
        f.run(1); // DIVIDE again with no fresh block fails
        CHECK(f.cpu().err == 1);
        CHECK(soup_ok(f.s));
    }
    {
        Fix f; // CX = 0 gives nothing; BX != 0 is dispersal, off until Phase 3
        CHECK(f.init("ALLOC DIVIDE"));
        f.regs(0, 0, 4);
        f.run(1);
        f.regs(0, 1, 32);
        f.run(1);
        CHECK(f.cpu().err == 1 && f.s.tally().births == 0);
        f.cpu().ip = 101;
        f.regs(0, 0, 0);
        f.run(1);
        u32 d = f.s.body_at(1);
        CHECK(f.s.tally().births == 1 && f.s.body(d).units == 0);
        f.s.death_sweep(); // born with no CPU: effectively waste, and swept
        CHECK(f.s.body_count() == 1 && f.s.waste() == 1022);
        CHECK(soup_ok(f.s));
    }
    {
        Fix f; // RELEASE: units keep their heat; memory from the latest block
        CHECK(f.init("RELEASE"));
        f.regs(0, 1, 100);
        f.run(1);
        CHECK(f.s.body(f.b).units == 400 && f.s.free_units() == 600);
        f.regs(0, 0, 1);
        f.run(1);
        CHECK(f.cpu().err == 1); // no allocated block to release
        CHECK(soup_ok(f.s));
    }
    {
        Fix f; // signals, disk and GPU are off in Phase 1a
        CHECK(f.init("EMIT LISTEN DISK GPU"));
        for (u32 i = 0; i < 4; ++i) {
            f.run(1);
            CHECK(f.cpu().err == 1);
        }
    }
    {
        Fix f; // YIELD gives up the rest of the tick
        CHECK(f.init("YIELD INC"));
        CHECK(f.s.run_cpu(f.c, 10) == 1);
        CHECK(f.cpu().yielded == 1 && f.cpu().cx == 0);
    }
    {
        Fix f; // RAND: an address-sized value
        CHECK(f.init("RAND"));
        bool in_range = true;
        for (u32 i = 0; i < 200; ++i) {
            f.run(1);
            in_range = in_range && f.cpu().cx < 1024;
        }
        CHECK(in_range);
    }
}

} // namespace

void suite_interp() {
    registers();
    conditionals();
    stack();
    templates();
    bounds_and_wrap();
    world_verbs();
}
