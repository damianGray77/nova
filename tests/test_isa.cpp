// Codon tables (design.md 6.2): the v1 layout and its mutation geometry, the
// tierra0 table, and custom tables from config.
#include "check.h"
#include "nova/isa.h"

using namespace nova;

namespace {

void nova_v1_layout() {
    CodonTable t;
    CHECK(builtin_table("nova_v1", &t));
    // The 6.2 table, slot by slot.
    const char* const core[16] = {"NOP0", "NOP1", "JMP",  "IFZ",  "INC",   "DEC",   "ZERO",  "ROT",
                                  "PUSH", "POP",  "COPY", "FIND", "IFLT", "SENSE", "ALLOC", "DIVIDE"};
    for (u32 k = 0; k < 16; ++k) {
        CHECK(Str(op_name(t.op[2 * k])).eq(core[k]));
        CHECK(Str(op_name(t.op[2 * k + 1])).eq(core[k]));
    }
    const char* const rich[32] = {"CALL",   "RET",    "DJNZ", "JZ",     "ADD",  "SUB",  "MUL",
                                  "DIV",    "AND",    "OR",   "XOR",    "NOT",  "SHL",  "SHR",
                                  "NEG",    "RAND",   "IFNE", "IFERR",  "SWAP", "ROTB", "SETH",
                                  "GETH",   "READ",   "WRITE", "ENGULF", "INJECT", "EMIT", "LISTEN",
                                  "DISK",   "GPU",    "YIELD", "RELEASE"};
    for (u32 k = 0; k < 32; ++k)
        CHECK(Str(op_name(t.op[32 + k])).eq(rich[k]));

    // Mutation geometry: bit 0 is synonymous in the core; bit 1 flips a core
    // instruction to its listed partner; bit 5 moves between core and rich.
    bool bit0 = true, bit1 = true, bit5 = true;
    for (u32 c = 0; c < 32; ++c) {
        bit0 = bit0 && t.op[c] == t.op[c ^ 1];
        u32 k = static_cast<u32>(t.op[c]), kp = static_cast<u32>(t.op[c ^ 2]);
        bit1 = bit1 && (k ^ 1) == kp;
        bit5 = bit5 && static_cast<u32>(t.op[c ^ 32]) >= static_cast<u32>(Op::call);
    }
    CHECK(bit0);
    CHECK(bit1);
    CHECK(bit5);
    // 48 distinct instructions; every core one twice, every rich one once.
    u32 seen[kOpCount] = {};
    for (u32 c = 0; c < 64; ++c)
        ++seen[static_cast<u32>(t.op[c])];
    u32 distinct = 0;
    for (u32 i = 0; i < kOpCount; ++i)
        distinct += seen[i] != 0;
    CHECK(distinct == 48);
    CHECK(seen[static_cast<u32>(Op::copy)] == 2 && seen[static_cast<u32>(Op::engulf)] == 1);
}

void tierra0_layout() {
    CodonTable t;
    CHECK(builtin_table("tierra0", &t));
    // Ray's hex codes as used in the ancestor listing (Ray 1991, App. C).
    CHECK(t.op[0x00] == Op::nop0 && t.op[0x01] == Op::nop1);
    CHECK(t.op[0x02] == Op::t_not0 && t.op[0x05] == Op::t_ifz);
    CHECK(t.op[0x06] == Op::t_subcab && t.op[0x07] == Op::t_subaac);
    CHECK(t.op[0x14] == Op::t_jmpo && t.op[0x16] == Op::t_call && t.op[0x17] == Op::t_ret);
    CHECK(t.op[0x18] == Op::t_movdc && t.op[0x19] == Op::t_movba && t.op[0x1a] == Op::t_movii);
    CHECK(t.op[0x1c] == Op::t_adrb && t.op[0x1d] == Op::t_adrf);
    CHECK(t.op[0x1e] == Op::t_mal && t.op[0x1f] == Op::t_divide);
    for (u32 c = 32; c < 64; ++c)
        CHECK(t.op[c] == t.op[c - 32]);
}

void names_and_custom() {
    Op op;
    CHECK(op_from_name("COPY", &op) && op == Op::copy);
    CHECK(op_from_name("movii", &op) && op == Op::t_movii);
    CHECK(op_from_name("nop1", &op) && op == Op::nop1); // Tierra spelling aliases NOP1
    CHECK(!op_from_name("copy", &op));                  // case-sensitive
    CHECK(!op_from_name("BOGUS", &op));

    // Round trip: nova_v1 written out parses back to the same table.
    CodonTable v1, back;
    CHECK(builtin_table("nova_v1", &v1));
    TextBuf text;
    write_table(v1, text);
    TextBuf err;
    CHECK(parse_table(text.view(), &back, &err));
    bool same = true;
    for (u32 c = 0; c < 64; ++c)
        same = same && v1.op[c] == back.op[c];
    CHECK(same);
    CHECK(Str(back.name).eq("custom"));

    // A doubled-INJECT variant (a Phase 1b experiment) is just config.
    TextBuf variant;
    for (u32 c = 0; c < 64; ++c) {
        if (c)
            variant.append_char(' ');
        variant.append(c == 59 ? "INJECT" : op_name(v1.op[c])); // LISTEN's slot
    }
    CodonTable dbl;
    CHECK(parse_table(variant.view(), &dbl, &err));
    CHECK(dbl.op[57] == Op::inject && dbl.op[59] == Op::inject);
    CHECK(codon_of(dbl, Op::inject) == 57);
    CHECK(codon_of(dbl, Op::listen) == -1);

    TextBuf e1, e2;
    CHECK(!parse_table("NOP0 NOP1", &back, &e1));
    CHECK(Str(e1.c_str()).starts_with("expected 64 codons"));
    CHECK(!parse_table("NOP0 bogus", &back, &e2));
    CHECK(Str(e2.c_str()).starts_with("unknown instruction 'bogus'"));
}

} // namespace

void suite_isa() {
    nova_v1_layout();
    tierra0_layout();
    names_and_custom();
}
