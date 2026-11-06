// The warp interpreter (Phase 8, design.md 6.14): ONE source, compiled as
// C++ for the CPU reference (warp.cpp) and as HLSL for the GPU (8b), so the
// two cannot drift. It is written in the subset the two languages share:
// plain functions, 32-bit integers, no pointers. Rules that keep it so:
//   - HLSL evaluates both sides of && and || and both arms of ?: , so no
//     operand of those may have a side effect (an ATOMIC_ADD, a write);
//     reads are fine (their value is masked off when unused).
//   - functions are defined before use (HLSL), all temporaries declared at
//     the top of a function (HLSL switch cases can't declare).
//
// The includer provides:
//   uint                 32-bit unsigned
//   INOUT(T)             inout T (HLSL) / T& (C++)
//   IMG(i)               word i of this batch's image (read and write)
//   CST(i)               word i of the constants
//   LANES(i)             lane i of this batch's lanes
//   WARP_LANE0(wr, wi)   where warp wi's lanes start in LANES: its slot's, WW_LANE0
//                        (the GPU keeps each node's lanes by slot too)
//   ATOMIC_ADD(i, v)     IMG(i) += v, atomically on the GPU
//   SYNC                 a thread-group barrier (nothing in C++)
//   ONCE_BEGIN/ONCE_END  code that one thread of the group runs
//   LANE_BEGIN(l0)/LANE_END   per lane l (a thread each on the GPU, a loop in
//                        C++), with the lane's state as L
//   LANES_LOAD(l0)/LANES_STORE  load L before / store it after (GPU only)
//   LANE_T               the lane's registers as functions take them (C++:
//                        Lane; GPU: the registers without the stack)
//   STACK_GET(L, i) / STACK_SET(L, i, v)  the lane's stack (C++: L.stack; GPU:
//                        the lane's stack in memory, since a local array
//                        indexed at run time can't live in a barrier loop)
//   GS_MIN(x, v) / GS_ADD(x, v) groupshared atomics on gs_min, gs_count
//   gs_min, gs_count, gs_wa[], gs_wv[], gs_ctl[8]  groupshared words
//   W_LOOP               [loop] (HLSL) / nothing
//   struct Lane          as nova::Lane (soup.h)
//
// What a warp is (6.14):
//   - Lockstep, "min-IP first": each step runs the instruction at the lowest
//     IP among the lanes still running, for every lane at that IP. Lanes that
//     agree run together; lanes that branch apart take turns; lanes that
//     reach the same address again merge by themselves.
//   - A warp's world is its own body: lanes fetch, read, write and search
//     only in codons their body owns. Reads of waste or of others' code fail.
//   - Within a step every lane reads before any lane writes; when two lanes
//     write one codon, the highest lane's value stays.
//   - Heads are offset by lane and move by the lane count: COPY, READ and
//     WRITE advance a head by the number of lanes; SETH puts lane L's head L
//     past AX and GETH reports it L back. An address inside one of the
//     body's blocks moves within that block, wrapping at its end; one outside
//     moves along the soup.
//   - World-changing instructions are the host's: the warp stops in front of
//     one and the host runs it after the GPU phase (warp.cpp).
//   - Random draws are keyed by (body key, warp, step, lane, purpose), never
//     drawn from a shared stream.

// The layout (offsets, bits, Op numbers) is warp_layout.inc, which the
// includer includes first.

// --- arithmetic -------------------------------------------------------------------

// A 32-bit integer hash (lowbias32): every output bit depends on every input bit.
uint w_mix(uint x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint w_draw(uint key, uint step, uint lane, uint what) {
    return w_mix(w_mix(key + step * 0x9E3779B9u) ^ (lane * 0x85EBCA6Bu + what * 0xC2B2AE35u));
}

// floor(a x b / 2^32), with 32-bit arithmetic only (the GPU has no 64-bit).
uint w_mulhi(uint a, uint b) {
    uint a0 = a & 0xFFFFu;
    uint a1 = a >> 16;
    uint b0 = b & 0xFFFFu;
    uint b1 = b >> 16;
    uint p00 = a0 * b0;
    uint p01 = a0 * b1;
    uint p10 = a1 * b0;
    uint p11 = a1 * b1;
    uint mid = (p00 >> 16) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    return p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
}

// A uniform draw d mapped onto [0, n).
uint w_below(uint d, uint n) {
    return w_mulhi(d, n);
}

// Signed division and remainder as C does them (truncating), with unsigned
// operations only, so both compilers give the same bits. The caller handles
// b == 0 and INT_MIN / -1.
uint w_sdiv(uint a, uint b) {
    uint ua = a;
    uint ub = b;
    uint q = 0u;
    if ((a & 0x80000000u) != 0u)
        ua = 0u - a;
    if ((b & 0x80000000u) != 0u)
        ub = 0u - b;
    q = ua / ub;
    if (((a ^ b) & 0x80000000u) != 0u)
        q = 0u - q;
    return q;
}

uint w_smod(uint a, uint b) {
    uint ua = a;
    uint ub = b;
    uint r = 0u;
    if ((a & 0x80000000u) != 0u)
        ua = 0u - a;
    if ((b & 0x80000000u) != 0u)
        ub = 0u - b;
    r = ua % ub;
    if ((a & 0x80000000u) != 0u)
        r = 0u - r;
    return r;
}

// --- the body's memory ------------------------------------------------------------

// The block-table entry of the first of the body's blocks whose range holds
// address a, or W_NONE.
uint w_range(uint b, uint a) {
    uint n = IMG(b + WB_NBLK);
    uint t = IMG(b + WB_BLK);
    uint s = IMG(b + WB_SORT);
    uint j = 0u;
    uint e = 0u;
    uint lo = 0u;
    uint hi = 0u;
    uint mid = 0u;
    if (s != 0u) {
        // Blocks that don't overlap (the packer checked), indexed by start
        // (pairs of start, entry): at most one can hold a, the last that
        // starts at or before it -- the same answer as the scan below.
        hi = n;
        W_LOOP while (hi - lo > 1u) {
            mid = (lo + hi) >> 1;
            if (IMG(s + 2u * mid) <= a)
                lo = mid;
            else
                hi = mid;
        }
        e = IMG(s + 2u * lo + 1u);
        if (a - IMG(e + WK_START) < IMG(e + WK_LEN))
            return e;
        return W_NONE;
    }
    for (j = 0u; j < n; j++) {
        e = t + j * WK_SIZE;
        if (a - IMG(e + WK_START) < IMG(e + WK_LEN))
            return e;
    }
    return W_NONE;
}

uint w_word_at(uint e, uint a) {
    return IMG(e + WK_CODE) + (a - IMG(e + WK_START));
}

// The codon word of address a, or W_NONE outside the body's blocks.
uint w_word(uint b, uint a) {
    uint e = w_range(b, a);
    if (e == W_NONE)
        return W_NONE;
    return w_word_at(e, a);
}

bool w_mine(uint b, uint a) {
    uint w = w_word(b, a);
    if (w == W_NONE)
        return false;
    return (IMG(w) & CW_MINE) != 0u;
}

uint w_page(uint e, uint a) {
    return IMG(e + WK_PAGE) + ((a >> 6) - (IMG(e + WK_START) >> 6)) * WP_SIZE;
}

// n codons on from a: within the block whose range holds a (wrapping at its
// end), else along the soup.
uint w_adv(uint b, uint a, uint n) {
    uint e = w_range(b, a);
    uint s = 0u;
    uint len = 0u;
    uint size = IMG(b + WB_SIZE);
    if (e == W_NONE)
        return (a + n % size) % size;
    s = IMG(e + WK_START);
    len = IMG(e + WK_LEN);
    return s + (a - s + n % len) % len;
}

// n codons back from a, the mirror of w_adv.
uint w_back(uint b, uint a, uint n) {
    uint e = w_range(b, a);
    uint s = 0u;
    uint len = 0u;
    uint size = IMG(b + WB_SIZE);
    if (e == W_NONE)
        return (a + size - n % size) % size;
    s = IMG(e + WK_START);
    len = IMG(e + WK_LEN);
    return s + (a - s + len - n % len) % len;
}

// The address after an instruction at a and its n-codon template.
uint w_skip(uint b, uint a, uint n) {
    uint i = 0u;
    for (i = 0u; i <= n; i++)
        a = w_adv(b, a, 1u);
    return a;
}

// A codon taken through a head (word w), with the salvage rule: a codon
// stamped with a genome length above the limit may come out garbled.
uint w_take(uint b, uint w, uint key, uint step, uint lane) {
    uint v = IMG(w) & CW_CODON;
    uint delta = IMG(b + WB_STAMP);
    uint st = 0u;
    uint lim = 0u;
    if (delta == 0u)
        return v;
    st = IMG(w + delta);
    lim = IMG(b + WB_SALV_LIM);
    if (st == 0u || st <= lim)
        return v;
    if (w_mulhi(w_draw(key, step, lane, WD_SALV), st) < lim)
        return v; // through intact with chance lim / st
    ATOMIC_ADD(b + WB_OUT + WO_GARBLED, 1u);
    return w_draw(key, step, lane, WD_SALV_V) & 63u;
}

// A copied codon, with the chance of a one-bit copy error.
uint w_copied(uint b, uint v, uint key, uint step, uint lane) {
    if (w_draw(key, step, lane, WD_COPY) < IMG(b + WB_COPY_THR)) {
        v ^= 1u << w_below(w_draw(key, step, lane, WD_COPY_BIT), 6u);
        ATOMIC_ADD(b + WB_OUT + WO_COPYERR, 1u);
    }
    return v;
}

// Does the page holding a (one of the body's) accept a write? A hot one
// refuses with chance 1 - its speed factor.
bool w_accepts(uint b, uint a, uint key, uint step, uint lane) {
    uint e = w_range(b, a);
    uint thr = 0u;
    if (e == W_NONE)
        return false;
    thr = IMG(w_page(e, a) + WP_THR);
    if (thr == W_NONE)
        return true;
    return w_draw(key, step, lane, WD_PAGE) < thr;
}

// Can a lane write address a: the body's own, and the page accepts.
bool w_writable(uint b, uint a, uint key, uint step, uint lane) {
    if (!w_mine(b, a))
        return false;
    return w_accepts(b, a, key, step, lane);
}

// --- templates --------------------------------------------------------------------

uint w_bit(uint t0, uint t1, uint i) {
    if (i < 32u)
        return (t0 >> i) & 1u;
    return (t1 >> (i - 32u)) & 1u;
}

// The run of NOPs from `from` in the body's own code, at most max_template
// long: its bits into t0 (bits 0-31) and t1 (32-63), its length returned.
uint w_template(uint b, uint from, INOUT(uint) t0, INOUT(uint) t1) {
    uint n = 0u;
    uint a = from;
    uint maxt = CST(WC_TEMPLATE);
    uint w = 0u;
    uint op = 0u;
    t0 = 0u;
    t1 = 0u;
    W_LOOP while (n < maxt) {
        w = w_word(b, a);
        if (w == W_NONE)
            break;
        if ((IMG(w) & CW_MINE) == 0u)
            break;
        op = CST(WC_TABLE + (IMG(w) & CW_CODON));
        if (op != OP_NOP0 && op != OP_NOP1)
            break;
        if (op == OP_NOP1) {
            if (n < 32u)
                t0 |= 1u << n;
            else
                t1 |= 1u << (n - 32u);
        }
        n++;
        a = w_adv(b, a, 1u);
    }
    return n;
}

bool w_complement_at(uint b, uint start, uint t0, uint t1, uint n) {
    uint size = IMG(b + WB_SIZE);
    uint i = 0u;
    uint a = 0u;
    uint w = 0u;
    uint op = 0u;
    uint bit = 0u;
    for (i = 0u; i < n; i++) {
        a = start + i;
        if (a >= size)
            a -= size;
        w = w_word(b, a);
        if (w == W_NONE)
            return false;
        if ((IMG(w) & CW_MINE) == 0u)
            return false;
        op = CST(WC_TABLE + (IMG(w) & CW_CODON));
        if (op != OP_NOP0 && op != OP_NOP1)
            return false;
        bit = 0u;
        if (op == OP_NOP1)
            bit = 1u;
        if (bit == w_bit(t0, t1, i))
            return false;
    }
    return true;
}

// The complement of an n-codon template following the instruction at a,
// searched within the body's span: dir 0 outward (forward first at each
// distance), 1 forward only, -1 backward only. Returns the address after the
// match, or W_NONE. (The CPU's private-code search, design.md 6.5.)
uint w_search(uint b, uint a, uint t0, uint t1, uint n, int dir) {
    int size = (int)IMG(b + WB_SIZE);
    int lo = (int)IMG(b + WB_LO);
    int hi = (int)IMG(b + WB_HI);
    int lim = (int)CST(WC_SEARCH);
    int fmax = lim;
    int bmax = lim;
    int dmax = 0;
    int d = 0;
    int st = 0;
    int f2 = hi - (int)n - (int)a - (int)n;
    int b2 = (int)a - (int)n + 1 - lo;
    if (f2 < fmax)
        fmax = f2;
    if (b2 < bmax)
        bmax = b2;
    if (dir >= 0 && fmax > dmax)
        dmax = fmax;
    if (dir <= 0 && bmax > dmax)
        dmax = bmax;
    W_LOOP for (d = 1; d <= dmax; d++) {
        if (dir >= 0 && d <= fmax) {
            st = (int)a + (int)n + d;
            if (st + (int)n <= size) {
                if (w_complement_at(b, (uint)st, t0, t1, n))
                    return w_adv(b, ((uint)st + n - 1u) % (uint)size, 1u);
            }
        }
        if (dir <= 0 && d <= bmax) {
            st = (int)a - (int)n - d + 1;
            if (st >= 0) {
                if (w_complement_at(b, (uint)st, t0, t1, n))
                    return w_adv(b, ((uint)st + n - 1u) % (uint)size, 1u);
            }
        }
    }
    return W_NONE;
}

// A template jump: on success next is the landing address; on failure it
// skips the template and returns false.
bool w_jump(uint b, uint ip, int dir, INOUT(uint) next) {
    uint t0 = 0u;
    uint t1 = 0u;
    uint n = w_template(b, w_adv(b, ip, 1u), t0, t1);
    uint after = w_skip(b, ip, n);
    uint to = W_NONE;
    if (n == 0u) {
        next = after;
        return false; // no template, no target
    }
    to = w_search(b, ip, t0, t1, n, dir);
    next = after;
    if (to != W_NONE)
        next = to;
    return to != W_NONE;
}

// --- a lane ----------------------------------------------------------------------

bool w_push(INOUT(LANE_T) L, uint v) {
    if (L.sp >= CST(WC_STACK))
        return false;
    STACK_SET(L, L.sp, v);
    L.sp++;
    return true;
}

bool w_pop(INOUT(LANE_T) L, INOUT(uint) v) {
    if (L.sp == 0u)
        return false;
    L.sp--;
    v = STACK_GET(L, L.sp);
    return true;
}

void w_fault(uint b, INOUT(LANE_T) L) {
    L.err = 1u;
    if (L.errs < 255u)
        L.errs++;
    if (L.errs >= CST(WC_HALT) && L.halted == 0u) {
        L.halted = 1u;
        ATOMIC_ADD(b + WB_OUT + WO_HALTS, 1u);
    }
}

// The length of the block the lane runs in (SENSE 0), 0 if it has none.
uint w_block_len(uint b, uint id) {
    uint n = IMG(b + WB_NBLK);
    uint t = IMG(b + WB_BLK);
    uint j = 0u;
    for (j = 0u; j < n; j++) {
        if (IMG(t + j * WK_SIZE + WK_ID) == id)
            return IMG(t + j * WK_SIZE + WK_LEN);
    }
    return 0u;
}

// Is op the host's (world-changing): the warp stops in front of it.
bool w_host_op(uint op) {
    return op == OP_ALLOC || op == OP_DIVIDE || op == OP_ENGULF || op == OP_INJECT || op == OP_EMIT ||
           op == OP_LISTEN || op == OP_DISK || op == OP_GPU || op == OP_RELEASE || op == OP_T_MAL ||
           op == OP_T_DIVIDE;
}

// One lane's instruction (never a host one) at ip, reading memory only: a
// write goes to wto / wval for the step's write phase. Returns ok; next is
// the address after the instruction on entry and where the lane goes on exit.
bool w_exec(uint b, INOUT(LANE_T) L, uint lane, uint op, uint ip, uint key, uint step, INOUT(uint) next,
            INOUT(uint) wto, INOUT(uint) wval) {
    uint lanes = CST(WC_LANES);
    uint size = IMG(b + WB_SIZE);
    bool ok = true;
    uint t0 = 0u;
    uint t1 = 0u;
    uint n = 0u;
    uint to = W_NONE;
    uint v = 0u;
    uint x = 0u;
    uint y = 0u;
    uint r = 0u;
    uint fw = 0u;
    uint e = 0u;
    switch (op) {
    case OP_NOP0:
    case OP_NOP1:
    case OP_NOPX:
        break;

    case OP_JMP:
    case OP_T_JMPO:
        ok = w_jump(b, ip, 0, next);
        break;
    case OP_JMPF:
        ok = w_jump(b, ip, 1, next);
        break;
    case OP_JMPB:
    case OP_T_JMPB:
        ok = w_jump(b, ip, -1, next);
        break;
    case OP_IFZ:
    case OP_T_IFZ:
        if (L.cx != 0u)
            next = w_adv(b, next, 1u);
        break;
    case OP_INC:
    case OP_T_INCC:
        L.cx++;
        break;
    case OP_DEC:
    case OP_T_DECC:
        L.cx--;
        break;
    case OP_ZERO:
    case OP_T_ZERO:
        L.cx = 0u;
        break;
    case OP_ROT: // AX -> BX -> CX -> AX
        x = L.ax;
        y = L.bx;
        v = L.cx;
        L.bx = x;
        L.cx = y;
        L.ax = v;
        break;
    case OP_PUSH:
    case OP_T_PUSHCX:
        ok = w_push(L, L.cx);
        break;
    case OP_POP:
    case OP_T_POPCX: // (through v: HLSL copies inout arguments back, so never pass a part of L)
        ok = w_pop(L, v);
        if (ok)
            L.cx = v;
        break;

    case OP_COPY:
        fw = w_word(b, L.rh);
        ok = false;
        if (fw != W_NONE) {
            if ((IMG(fw) & CW_MINE) != 0u)
                ok = w_writable(b, L.wh, key, step, lane);
        }
        if (ok) {
            wto = L.wh;
            wval = w_copied(b, w_take(b, fw, key, step, lane), key, step, lane);
            L.rh = w_adv(b, L.rh, lanes);
            L.wh = w_adv(b, L.wh, lanes);
        }
        break;
    case OP_READ:
        fw = w_word(b, L.rh);
        ok = false;
        if (fw != W_NONE)
            ok = (IMG(fw) & CW_MINE) != 0u;
        if (ok) {
            L.cx = w_take(b, fw, key, step, lane);
            L.rh = w_adv(b, L.rh, lanes);
        }
        break;
    case OP_WRITE:
        ok = w_writable(b, L.wh, key, step, lane);
        if (ok) {
            wto = L.wh;
            wval = L.cx & 63u;
            L.wh = w_adv(b, L.wh, lanes);
        }
        break;
    case OP_T_MOVII: // [AX] <- [BX], no lane offsets
        x = L.ax % size;
        y = L.bx % size;
        fw = w_word(b, y);
        ok = false;
        if (fw != W_NONE) {
            if ((IMG(fw) & CW_MINE) != 0u)
                ok = w_writable(b, x, key, step, lane);
        }
        if (ok) {
            wto = x;
            wval = w_copied(b, w_take(b, fw, key, step, lane), key, step, lane);
        }
        break;

    case OP_FIND:
        n = w_template(b, next, t0, t1);
        next = w_skip(b, ip, n);
        to = W_NONE;
        if (n != 0u)
            to = w_search(b, ip, t0, t1, n, 0);
        ok = to != W_NONE;
        if (ok)
            L.ax = to;
        break;
    case OP_T_ADRO:
    case OP_T_ADRB:
    case OP_T_ADRF:
        n = w_template(b, next, t0, t1);
        next = w_skip(b, ip, n);
        to = W_NONE;
        if (n != 0u) {
            if (op == OP_T_ADRO)
                to = w_search(b, ip, t0, t1, n, 0);
            else if (op == OP_T_ADRB)
                to = w_search(b, ip, t0, t1, n, -1);
            else
                to = w_search(b, ip, t0, t1, n, 1);
        }
        ok = to != W_NONE;
        if (ok) {
            L.ax = to;
            L.cx = n;
        }
        break;
    case OP_CALL:
    case OP_T_CALL:
        n = w_template(b, next, t0, t1);
        x = w_skip(b, ip, n); // where RET comes back to
        to = W_NONE;
        if (n != 0u)
            to = w_search(b, ip, t0, t1, n, 0);
        ok = false;
        if (to != W_NONE)
            ok = w_push(L, x);
        next = x;
        if (ok)
            next = to;
        break;
    case OP_RET:
    case OP_T_RET:
        ok = w_pop(L, v);
        if (ok)
            next = v % size;
        break;
    case OP_DJNZ:
        L.cx--;
        if (L.cx != 0u) {
            ok = w_jump(b, ip, 0, next);
        } else {
            n = w_template(b, next, t0, t1);
            next = w_skip(b, ip, n);
        }
        break;
    case OP_JZ:
        if (L.cx == 0u) {
            ok = w_jump(b, ip, 0, next);
        } else {
            n = w_template(b, next, t0, t1);
            next = w_skip(b, ip, n);
        }
        break;

    case OP_IFLT:
        if (!((int)L.ax < (int)L.bx))
            next = w_adv(b, next, 1u);
        break;
    case OP_IFGT:
        if (!((int)L.ax > (int)L.bx))
            next = w_adv(b, next, 1u);
        break;
    case OP_IFNE:
        if (L.ax == L.bx)
            next = w_adv(b, next, 1u);
        break;
    case OP_IFEQ:
        if (L.ax != L.bx)
            next = w_adv(b, next, 1u);
        break;
    case OP_IFERR: // tests the previous instruction's flag
        if (L.err == 0u)
            next = w_adv(b, next, 1u);
        break;

    case OP_ADD:
        L.cx = L.ax + L.bx;
        break;
    case OP_SUB:
    case OP_T_SUBCAB:
        L.cx = L.ax - L.bx;
        break;
    case OP_T_SUBAAC:
        L.ax = L.ax - L.cx;
        break;
    case OP_MUL:
        L.cx = L.ax * L.bx;
        break;
    case OP_DIV:
        if (L.bx == 0u)
            ok = false;
        else if (L.ax == 0x80000000u && L.bx == 0xFFFFFFFFu)
            L.cx = L.ax;
        else
            L.cx = w_sdiv(L.ax, L.bx);
        break;
    case OP_MOD:
        if (L.bx == 0u)
            ok = false;
        else if (L.ax == 0x80000000u && L.bx == 0xFFFFFFFFu)
            L.cx = 0u;
        else
            L.cx = w_smod(L.ax, L.bx);
        break;
    case OP_AND:
        L.cx = L.ax & L.bx;
        break;
    case OP_OR:
        L.cx = L.ax | L.bx;
        break;
    case OP_XOR:
        L.cx = L.ax ^ L.bx;
        break;
    case OP_NOT:
        L.cx = ~L.cx;
        break;
    case OP_T_NOT0:
        L.cx ^= 1u;
        break;
    case OP_SHL:
    case OP_T_SHL:
        L.cx = L.cx << 1;
        break;
    case OP_SHR:
        L.cx = L.cx >> 1;
        break;
    case OP_NEG:
        L.cx = 0u - L.cx;
        break;
    case OP_MIN:
        if ((int)L.ax < (int)L.bx)
            L.cx = L.ax;
        else
            L.cx = L.bx;
        break;
    case OP_MAX:
        if ((int)L.ax > (int)L.bx)
            L.cx = L.ax;
        else
            L.cx = L.bx;
        break;
    case OP_RAND: // a random address-sized value, from this lane's own draw
        L.cx = w_below(w_draw(key, step, lane, WD_RAND), size);
        break;
    case OP_SWAP:
        x = L.ax;
        L.ax = L.bx;
        L.bx = x;
        break;
    case OP_ROTB: // AX -> CX -> BX -> AX
        x = L.ax;
        y = L.bx;
        v = L.cx;
        L.ax = y;
        L.bx = v;
        L.cx = x;
        break;
    case OP_T_INCA:
    case OP_INCA:
        L.ax++;
        break;
    case OP_DECA:
        L.ax--;
        break;
    case OP_T_INCB:
    case OP_INCB:
        L.bx++;
        break;
    case OP_DECB:
        L.bx--;
        break;
    case OP_ZEROA:
        L.ax = 0u;
        break;
    case OP_ZEROB:
        L.bx = 0u;
        break;
    case OP_T_PUSHAX:
        ok = w_push(L, L.ax);
        break;
    case OP_T_PUSHBX:
        ok = w_push(L, L.bx);
        break;
    case OP_T_PUSHDX:
        ok = w_push(L, L.dx);
        break;
    case OP_T_POPAX:
        ok = w_pop(L, v);
        if (ok)
            L.ax = v;
        break;
    case OP_T_POPBX:
        ok = w_pop(L, v);
        if (ok)
            L.bx = v;
        break;
    case OP_T_POPDX:
        ok = w_pop(L, v);
        if (ok)
            L.dx = v;
        break;
    case OP_T_MOVDC:
        L.dx = L.cx;
        break;
    case OP_T_MOVBA:
        L.bx = L.ax;
        break;
    case OP_DUP: // push the top of the stack again
        ok = false;
        if (L.sp > 0u) {
            v = STACK_GET(L, L.sp - 1u);
            ok = w_push(L, v);
        }
        break;
    case OP_DROP:
        ok = w_pop(L, v);
        break;

    case OP_SETH: // lane L's head goes L codons past AX
        if (L.ax >= size || L.bx > 1u) {
            ok = false;
        } else {
            v = w_adv(b, L.ax, lane);
            if (CST(WC_HEAD + L.bx) == 0u)
                L.rh = v;
            else
                L.wh = v;
        }
        break;
    case OP_GETH: // ... and reads back L codons before it
        if (L.bx > 1u) {
            ok = false;
        } else {
            v = L.wh;
            if (CST(WC_HEAD + L.bx) == 0u)
                v = L.rh;
            L.ax = w_back(b, v, lane);
        }
        break;
    case OP_YIELD:
        L.yielded = 1u;
        break;
    case OP_SENSE:
        if (L.bx >= CST(WC_SENSE_N)) {
            ok = false;
        } else {
            r = CST(WC_SENSE + L.bx);
            if (r == 0u) { // my size: the block I run in
                L.cx = w_block_len(b, L.block);
            } else if (r == 4u) { // heat of the page I run in
                e = w_range(b, ip);
                L.cx = IMG(w_page(e, ip) + WP_HEAT);
            } else if (r == 20u) { // my lane
                L.cx = lane;
            } else if (r == 21u) { // lanes in my warp
                L.cx = lanes;
            } else if (((IMG(b + WB_SENSE_OK) >> r) & 1u) != 0u) {
                L.cx = IMG(b + WB_SENSE + r);
            } else {
                ok = false;
            }
        }
        break;

    default: // the host's instructions never get here; anything else fails
        ok = false;
        break;
    }
    return ok;
}

// The write phase: lane value v into address a (one of the body's).
void w_store(uint b, uint a, uint v) {
    uint e = w_range(b, a);
    uint w = w_word_at(e, a);
    uint delta = IMG(b + WB_STAMP);
    IMG(w) = (IMG(w) & ~CW_CODON) | (v & CW_CODON) | CW_WRITTEN;
    if (delta != 0u)
        IMG(w + delta) = 0u; // written: now the body's own code
    ATOMIC_ADD(w_page(e, a) + WP_WRITE, 1u);
    ATOMIC_ADD(b + WB_OUT + WO_WRITES, 1u);
}

// Runs warp wi of the body whose record is at b, for its quota: the lockstep
// rule, step by step. It stops when the quota is spent, when every lane has
// halted or yielded, or in front of a host instruction. Phase 9b: and when
// its own clock is out -- each step takes its instruction's time on a GPU
// lane, and a warp has a tick of time a tick (a GPU is slow at serial work).
void w_run_warp(uint b, uint wi) {
    uint wr = b + WB_WARPS + wi * WW_SIZE;
    uint lane0 = WARP_LANE0(wr, wi);
    uint quota = 0u;
    uint tleft = 0u;
    uint tused = 0u;
    uint tstep = 0u;
    uint key = w_mix(IMG(b + WB_KEY) ^ (IMG(wr + WW_INDEX) * 0x27D4EB2Fu));
    uint step = IMG(wr + WW_STEP0);
    uint nl = CST(WC_LANES);
    uint used = 0u;
    uint steps = 0u;
    uint lins = 0u;
    uint wait = 0u;
    uint wait_ip = 0u;
    uint minip = 0u;
    uint e = 0u;
    uint cw = 0u;
    uint op = 0u;
    uint obid = 0u;
    uint g = 0u;
    bool mine = false;
    // Every value that steers the loop is decided by one thread and handed to
    // all through gs_ctl, with a barrier after each hand-over: the GPU's
    // compiler only lets barriers sit inside loops whose control it can see
    // every thread agrees on. (In C++ these are plain assignments.)
    ONCE_BEGIN
        gs_ctl[0] = IMG(wr + WW_QUOTA);
        gs_ctl[7] = IMG(wr + WW_TLEFT);
    ONCE_END
    SYNC
    quota = gs_ctl[0];
    tleft = gs_ctl[7];
    LANES_LOAD(lane0)
    LANE_BEGIN(lane0)
        if (step == 0u)
            L.yielded = 0u; // its first steps this tick: yields end
    LANE_END
    SYNC
    W_LOOP while (used < quota) {
        SYNC
        ONCE_BEGIN
            gs_min = W_NONE;
            gs_count = 0u;
        ONCE_END
        SYNC
        LANE_BEGIN(lane0)
            if (L.halted == 0u && L.yielded == 0u)
                GS_MIN(gs_min, L.ip);
        LANE_END
        SYNC
        ONCE_BEGIN // the step's instruction: the lowest IP, what is there
            minip = gs_min;
            e = W_NONE;
            cw = W_NONE;
            mine = false;
            op = OP_COUNT;
            obid = W_NONE;
            if (minip != W_NONE) {
                e = w_range(b, minip);
                if (e != W_NONE)
                    cw = w_word_at(e, minip);
                if (cw != W_NONE)
                    mine = (IMG(cw) & CW_MINE) != 0u;
                if (mine) {
                    op = CST(WC_TABLE + (IMG(cw) & CW_CODON));
                    obid = IMG(IMG(b + WB_BLK) + ((IMG(cw) >> CW_OWNER_SHIFT) & 0xFFFFu) * WK_SIZE + WK_ID);
                }
            }
            gs_ctl[0] = minip;
            gs_ctl[1] = op;
            gs_ctl[2] = 0u;
            if (mine)
                gs_ctl[2] = 1u;
            gs_ctl[3] = obid;
            gs_ctl[4] = e;
        ONCE_END
        SYNC
        minip = gs_ctl[0];
        op = gs_ctl[1];
        mine = gs_ctl[2] != 0u;
        obid = gs_ctl[3];
        e = gs_ctl[4];
        SYNC
        if (minip == W_NONE)
            break; // every lane halted or yielded
        if (mine && w_host_op(op)) {
            wait = 1u;
            wait_ip = minip;
            break;
        }
        tstep = CST(WC_OPTIME + OP_COUNT);
        if (mine)
            tstep = CST(WC_OPTIME + op);
        if (tused + tstep > tleft)
            break; // its clock is out for this tick
        tused += tstep;
        if (e != W_NONE) {
            ONCE_BEGIN
                IMG(w_page(e, minip) + WP_FETCH) += 1u; // one fetch serves every lane
            ONCE_END
        }
        LANE_BEGIN(lane0)
            gs_wa[l] = W_NONE;
            gs_wv[l] = 0u;
            if (L.halted == 0u && L.yielded == 0u && L.ip == minip) {
                uint nx = w_adv(b, minip, 1u);
                uint wto = W_NONE;
                uint wval = 0u;
                bool ok = false;
                uint fd = 0u;
                uint fdelta = 0u;
                uint freg = 0u;
                GS_ADD(gs_count, 1u);
                if (!mine) { // not the body's code: a lane can't run it
                    w_fault(b, L);
                    L.ip = nx;
                } else {
                    L.block = obid;
                    ok = w_exec(b, L, l, op, minip, key, step, nx, wto, wval);
                    // an execution flaw: one register comes out off by one
                    if (w_draw(key, step, l, WD_FLAW) < IMG(b + WB_FLAW_THR)) {
                        fd = w_draw(key, step, l, WD_FLAW_KIND);
                        fdelta = 0xFFFFFFFFu;
                        if ((fd & 1u) != 0u)
                            fdelta = 1u;
                        freg = w_below(fd, 3u);
                        if (freg == 0u)
                            L.ax += fdelta;
                        else if (freg == 1u)
                            L.bx += fdelta;
                        else
                            L.cx += fdelta;
                        ATOMIC_ADD(b + WB_OUT + WO_FLAWS, 1u);
                    }
                    if (ok) {
                        L.err = 0u;
                        L.errs = 0u;
                    } else {
                        w_fault(b, L);
                    }
                    L.ip = nx;
                    gs_wa[l] = wto;
                    gs_wv[l] = wval;
                }
            }
        LANE_END
        SYNC
        LANE_BEGIN(lane0)
            if (gs_wa[l] != W_NONE) {
                bool last = true;
                uint m = 0u;
                for (m = l + 1u; m < nl; m++) {
                    if (gs_wa[m] == gs_wa[l])
                        last = false; // a higher lane writes here too: its value stays
                }
                if (last)
                    w_store(b, gs_wa[l], gs_wv[l]);
            }
        LANE_END
        SYNC
        g = gs_count;
        SYNC
        used += CST(WC_STEP_COST) + CST(WC_LANE_COST) * g;
        steps++;
        lins += g;
        step++;
    }
    // How many lanes are left running, so the host need not look at them.
    SYNC
    ONCE_BEGIN
        gs_ctl[5] = 0u;
        gs_ctl[6] = 0u;
    ONCE_END
    SYNC
    LANE_BEGIN(lane0)
        if (L.halted == 0u) {
            GS_ADD(gs_ctl[5], 1u);
            if (L.yielded == 0u)
                GS_ADD(gs_ctl[6], 1u);
        }
    LANE_END
    SYNC
    LANES_STORE(lane0)
    ONCE_BEGIN
        IMG(wr + WW_USED) = used;
        IMG(wr + WW_TUSED) = tused;
        IMG(wr + WW_STEPS) = steps;
        IMG(wr + WW_LANEINS) = lins;
        IMG(wr + WW_WAIT) = wait;
        IMG(wr + WW_WAITIP) = wait_ip;
        IMG(wr + WW_LIVE) = gs_ctl[5];
        IMG(wr + WW_ACTIVE) = gs_ctl[6];
    ONCE_END
}

// Every warp of one body, one after another, in the body's order.
void w_run_body(uint b) {
    uint nw = 0u;
    uint wi = 0u;
    ONCE_BEGIN
        gs_ctl[0] = IMG(b + WB_NWARP);
    ONCE_END
    SYNC
    nw = gs_ctl[0];
    SYNC
    W_LOOP for (wi = 0u; wi < nw; wi++)
        w_run_warp(b, wi);
}
