#pragma once

// Instruction sets and codon tables (design.md 6.2, 6.5).
//
// The interpreter executes Ops. A CodonTable maps each of the 64 six-bit codons
// to an Op, and the table is chosen by config, so the same engine runs:
//
//   nova_v1        codon table v1 with the doubled core (design.md 6.2): 16 core
//                  instructions on two codons each (0-31), 32 rich ones on one each.
//   nova_v1_plain  the plain-64 control (Phase 1b D7): codon 2k is core k as in
//                  nova_v1, codon 2k+1 is the k-th of NOPX JMPF JMPB IFEQ IFGT MOD
//                  MIN MAX DUP DROP INCA DECA INCB DECB ZEROA ZEROB; 32-63 unchanged.
//   nova_v1_inv2   nova_v1 with ENGULF and INJECT doubled (Phase 1b D8): codons 58
//                  and 59 (EMIT and LISTEN, off until Phase 4) become ENGULF and
//                  INJECT, one bit-1 flip from 56 and 57.
//   nova_v2        nova_v1 with codon 0 INERT (Phase 10, design.md 6.16): what dead
//                  matter decays to. NOP0 keeps codon 1.
//   tierra0        Ray's original 32-opcode instruction set 0, for the light Tierra
//                  validation mode (design.md 9.1). Codons 32-63 repeat 0-31, since
//                  Tierra opcodes are 5 bits.
//   custom         any 64 Op names from config (isa.codons).
//
// Op names are case-sensitive: Nova's are upper case ("COPY"), Tierra's use the
// Tierra 4.1+ gb0 spelling ("movii", "subCAB"). Tierra's nop0/nop1 are the same
// Ops as NOP0/NOP1 (a template bit is a template bit), so both spellings parse.
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

enum class Op : u8 {
    // --- Nova codon table v1, core (doubled in nova_v1) ---------------------
    nop0, nop1, jmp, ifz, inc, dec, zero, rot,
    push, pop, copy, find, iflt, sense, alloc, divide,
    // --- Nova codon table v1, rich (one codon each) -------------------------
    call, ret, djnz, jz, add, sub, mul, div,
    and_, or_, xor_, not_, shl, shr, neg, rand,
    ifne, iferr, swap, rotb, seth, geth, read, write,
    engulf, inject, emit, listen, disk, gpu, yield, release,
    // --- Tierra instruction set 0 (nop0/nop1 shared with Nova) --------------
    t_not0, t_shl, t_zero, t_ifz, t_subcab, t_subaac, t_inca, t_incb,
    t_decc, t_incc, t_pushax, t_pushbx, t_pushcx, t_pushdx, t_popax, t_popbx,
    t_popcx, t_popdx, t_jmpo, t_jmpb, t_call, t_ret, t_movdc, t_movba,
    t_movii, t_adro, t_adrb, t_adrf, t_mal, t_divide,
    // --- Phase 1b plain-64 additions (design.md 6.6, D7) ----------------------
    nopx, jmpf, jmpb, ifeq, ifgt, mod, min_, max_,
    dup, drop, inca, deca, incb, decb, zeroa, zerob,
    // --- Phase 10: dead matter (nova_v2's codon 0). Not a NOP, so it ends
    // templates and surfaces; running it fails.
    inert,
    count
};

inline constexpr u32 kOpCount = static_cast<u32>(Op::count);

inline bool is_nop(Op op) {
    return op == Op::nop0 || op == Op::nop1;
}

const char* op_name(Op op);
// Parses a name (case-sensitive; Tierra's nop0/nop1 alias NOP0/NOP1).
bool op_from_name(Str name, Op* out);

struct CodonTable {
    Op op[64];
    char name[16]; // "nova_v1", "nova_v1_plain", "nova_v1_inv2", "tierra0" or "custom"
};

// Built-in tables by name; false for an unknown name.
bool builtin_table(Str name, CodonTable* out);
// Parses 64 whitespace-separated Op names. On failure writes a reason to err.
bool parse_table(Str codons, CodonTable* out, TextBuf* err);
// Writes the 64 names separated by single spaces (parse_table round-trips it).
void write_table(const CodonTable& t, TextBuf& out);
// First codon that decodes to op, or -1 if the table has none.
int codon_of(const CodonTable& t, Op op);

} // namespace nova
