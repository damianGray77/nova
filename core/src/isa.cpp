#include "nova/isa.h"

namespace nova {

namespace {

const char* const kNames[] = {
    // Nova core
    "NOP0", "NOP1", "JMP", "IFZ", "INC", "DEC", "ZERO", "ROT",
    "PUSH", "POP", "COPY", "FIND", "IFLT", "SENSE", "ALLOC", "DIVIDE",
    // Nova rich
    "CALL", "RET", "DJNZ", "JZ", "ADD", "SUB", "MUL", "DIV",
    "AND", "OR", "XOR", "NOT", "SHL", "SHR", "NEG", "RAND",
    "IFNE", "IFERR", "SWAP", "ROTB", "SETH", "GETH", "READ", "WRITE",
    "ENGULF", "INJECT", "EMIT", "LISTEN", "DISK", "GPU", "YIELD", "RELEASE",
    // Tierra instruction set 0, Tierra 4.1+ gb0 spelling
    "not0", "shl", "zero", "ifz", "subCAB", "subAAC", "incA", "incB",
    "decC", "incC", "pushA", "pushB", "pushC", "pushD", "popA", "popB",
    "popC", "popD", "jmpo", "jmpb", "call", "ret", "movDC", "movBA",
    "movii", "adro", "adrb", "adrf", "mal", "divide",
    // Phase 1b plain-64 additions
    "NOPX", "JMPF", "JMPB", "IFEQ", "IFGT", "MOD", "MIN", "MAX",
    "DUP", "DROP", "INCA", "DECA", "INCB", "DECB", "ZEROA", "ZEROB",
};
static_assert(sizeof kNames / sizeof kNames[0] == kOpCount, "one name per Op");

// The plain-64 partners, in codon order 1, 3, 5, ... 31 (design.md 6.6, D7).
const Op kPlainPartners[16] = {
    Op::nopx, Op::jmpf, Op::jmpb, Op::ifeq, Op::ifgt,  Op::mod,  Op::min_,  Op::max_,
    Op::dup,  Op::drop, Op::inca, Op::deca, Op::incb, Op::decb, Op::zeroa, Op::zerob,
};

// Ray's set 0 in opcode order 0x00-0x1f (design.md 9.1; the hex codes of the
// ancestor listing in Ray 1991, Appendix C).
const Op kTierra0[32] = {
    Op::nop0,     Op::nop1,     Op::t_not0,   Op::t_shl,    Op::t_zero,   Op::t_ifz,
    Op::t_subcab, Op::t_subaac, Op::t_inca,   Op::t_incb,   Op::t_decc,   Op::t_incc,
    Op::t_pushax, Op::t_pushbx, Op::t_pushcx, Op::t_pushdx, Op::t_popax,  Op::t_popbx,
    Op::t_popcx,  Op::t_popdx,  Op::t_jmpo,   Op::t_jmpb,   Op::t_call,   Op::t_ret,
    Op::t_movdc,  Op::t_movba,  Op::t_movii,  Op::t_adro,   Op::t_adrb,   Op::t_adrf,
    Op::t_mal,    Op::t_divide,
};

void set_name(CodonTable* t, const char* name) {
    usize i = 0;
    for (; name[i] && i + 1 < sizeof t->name; ++i)
        t->name[i] = name[i];
    for (; i < sizeof t->name; ++i)
        t->name[i] = '\0';
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

} // namespace

const char* op_name(Op op) {
    u32 i = static_cast<u32>(op);
    return i < kOpCount ? kNames[i] : "?";
}

bool op_from_name(Str name, Op* out) {
    if (name.eq("nop0")) {
        *out = Op::nop0;
        return true;
    }
    if (name.eq("nop1")) {
        *out = Op::nop1;
        return true;
    }
    for (u32 i = 0; i < kOpCount; ++i) {
        if (name.eq(kNames[i])) {
            *out = static_cast<Op>(i);
            return true;
        }
    }
    return false;
}

bool builtin_table(Str name, CodonTable* out) {
    const bool plain = name.eq("nova_v1_plain"), inv2 = name.eq("nova_v1_inv2");
    if (name.eq("nova_v1") || plain || inv2) {
        // Core: codons 2k and 2k+1 both decode to core instruction k, so bit 0 is
        // synonymous and bit 1 flips to the listed partner (design.md 6.2).
        for (u32 c = 0; c < 32; ++c)
            out->op[c] = static_cast<Op>(c >> 1);
        // Rich: one codon each, partners differing in bit 0.
        for (u32 c = 32; c < 64; ++c)
            out->op[c] = static_cast<Op>(static_cast<u32>(Op::call) + (c - 32));
        if (plain) // every instruction on one codon: 2k+1 gets a new partner
            for (u32 k = 0; k < 16; ++k)
                out->op[2 * k + 1] = kPlainPartners[k];
        if (inv2) { // the EMIT/LISTEN slots, always failing until Phase 4
            out->op[58] = Op::engulf;
            out->op[59] = Op::inject;
        }
        set_name(out, plain ? "nova_v1_plain" : (inv2 ? "nova_v1_inv2" : "nova_v1"));
        return true;
    }
    if (name.eq("tierra0")) {
        for (u32 c = 0; c < 64; ++c)
            out->op[c] = kTierra0[c & 31];
        set_name(out, "tierra0");
        return true;
    }
    return false;
}

bool parse_table(Str codons, CodonTable* out, TextBuf* err) {
    CodonTable t;
    usize i = 0;
    u32 n = 0;
    while (i < codons.n) {
        while (i < codons.n && is_space(codons.p[i]))
            ++i;
        if (i == codons.n)
            break;
        usize start = i;
        while (i < codons.n && !is_space(codons.p[i]))
            ++i;
        Str word = codons.sub(start, i - start);
        if (n == 64) {
            err->append("more than 64 codons");
            return false;
        }
        if (!op_from_name(word, &t.op[n])) {
            err->append("unknown instruction '");
            err->append(word);
            err->append("' at codon ");
            err->append_u64(n);
            return false;
        }
        ++n;
    }
    if (n != 64) {
        err->append("expected 64 codons, got ");
        err->append_u64(n);
        return false;
    }
    set_name(&t, "custom");
    *out = t;
    return true;
}

void write_table(const CodonTable& t, TextBuf& out) {
    for (u32 c = 0; c < 64; ++c) {
        if (c)
            out.append_char(' ');
        out.append(op_name(t.op[c]));
    }
}

int codon_of(const CodonTable& t, Op op) {
    for (int c = 0; c < 64; ++c)
        if (t.op[c] == op)
            return c;
    return -1;
}

} // namespace nova
