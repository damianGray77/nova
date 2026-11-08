// Warps (Phase 8, design.md 6.14): the soup's side of programs on the GPU.
//
// A round of the warp phase:
//   1. warp_pack: every body with a warp to run gets a record in one flat
//      image of 32-bit words: its blocks, its codons (with ownership), its
//      pages (write thresholds, heat), its SENSE readings taken now, its
//      random key, and each warp's quota (warp_isa.inc has the layout);
//   2. the run: warp_isa.inc's w_run_body on each record, here (the CPU
//      reference) or on the GPU (8b), on the very same image and lanes;
//   3. warp_unpack: written codons, page heat, counters and each warp's
//      outcome back into the soup;
//   4. warp_host: the world-changing instructions warps stopped in front of,
//      done once each, in the schedule's order (interp.cpp, Soup::host_step).
// Nothing in the soup changes while warps run, and they touch only their own
// body's memory, so every record can run independently and in any order.
#include <cmath>
#include <cstdio> // snprintf (the debugging trace)
#include <cstring>
#include <intrin.h> // __cpuid, _xgetbv, and AVX2 (the reference's flaw draws, where the CPU has it)

#include "nova/lineage.h"
#include "nova/pool.h"    // clock_seconds (profiling)
#include "nova/profile.h" // behaviour profiles (A3): lane instructions of a followed body
#include "nova/soup.h"

#include "warp_layout.inc"

namespace nova {

namespace {
// Profiling: adds the time from here to the end of the scope to *slot (when on).
struct Lap {
    double* slot;
    double t0;
    Lap(bool on, double* s) : slot(on ? s : nullptr), t0(on ? clock_seconds() : 0) {}
    ~Lap() {
        if (slot)
            *slot += clock_seconds() - t0;
    }
};
} // namespace


// --- the interpreter, compiled as C++ --------------------------------------------

namespace {

// Debugging (warp.verify's account of a difference): one lane's instructions
// as the reference runs them, a line each. One thread, on demand.
struct WarpTrace {
    bool on = false;
    u32 lane0 = 0, lane = 0;
    TextBuf* out = nullptr;
};
WarpTrace g_trace;
u64* g_ophist = nullptr; // warp_op_histogram: one thread at a time

void warp_trace(u32 lane0, u32 l, u32 step, u32 ip, u32 op, const Lane& L, u32 seen) {
    if (g_ophist) { // per Op (kOpCount: a lane that couldn't run the code): lane-instructions, then places searched
        const u32 k = op < kOpCount ? op : kOpCount;
        ++g_ophist[k];
        if (op < kOpCount)
            g_ophist[kOpCount + 1 + k] += seen;
    }
    if (!g_trace.on || lane0 != g_trace.lane0 || l != g_trace.lane || !g_trace.out)
        return;
    char line[256];
    snprintf(line, sizeof line,
             "    step %u ip %u %s -> ax %08x bx %08x cx %08x dx %08x sp %u err %u errs %u halted %u yielded %u\n",
             step, ip, op < kOpCount ? op_name(static_cast<Op>(op)) : "(not its code)", L.ax, L.bx, L.cx, L.dx, L.sp,
             L.err, L.errs, L.halted, L.yielded);
    g_trace.out->append(line);
}

} // namespace

void warp_trace_begin(u32 lane0, u32 lane, TextBuf* out) {
    g_trace.on = true;
    g_trace.lane0 = lane0;
    g_trace.lane = lane;
    g_trace.out = out;
}

void warp_trace_end() {
    g_trace = WarpTrace{};
}

void warp_op_histogram(u64* counts) {
    g_ophist = counts;
}

namespace {

// The CPU reference's fast steps (WarpVM::w_fast_exec): each lane's flaw
// draw, warp_isa.inc's w_draw_k(ks, lane, WD_FLAW), as a mask of the lanes
// whose draw is under the threshold -- eight lanes at a time with AVX2 where
// the CPU has it (checked once; the bits are the scalar ones either way).
u32 flaw_mix(u32 x) { // w_mix
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

bool cpu_avx2() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7)
        return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!osxsave || !avx || (_xgetbv(0) & 6) != 6)
        return false;
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
}
const bool g_avx2 = cpu_avx2();

u64 flaw_mask(u32 ks, u32 nl, u32 thr, u32 what) {
    if (thr == 0)
        return 0; // (no draw is under 0)
    u64 m = 0;
    u32 l = 0;
    if (g_avx2) {
        const __m256i c1 = _mm256_set1_epi32(static_cast<int>(0x85EBCA6Bu));
        const __m256i base = _mm256_set1_epi32(static_cast<int>(ks ^ 0u));
        const __m256i wk = _mm256_set1_epi32(static_cast<int>(what * 0xC2B2AE35u));
        const __m256i m1 = _mm256_set1_epi32(static_cast<int>(0x7FEB352Du));
        const __m256i m2 = _mm256_set1_epi32(static_cast<int>(0x846CA68Bu));
        const __m256i sign = _mm256_set1_epi32(static_cast<int>(0x80000000u));
        const __m256i t = _mm256_xor_si256(_mm256_set1_epi32(static_cast<int>(thr)), sign);
        __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i eight = _mm256_set1_epi32(8);
        for (; l + 8 <= nl; l += 8) {
            __m256i x = _mm256_xor_si256(base, _mm256_add_epi32(_mm256_mullo_epi32(lane, c1), wk));
            x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 16));
            x = _mm256_mullo_epi32(x, m1);
            x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 15));
            x = _mm256_mullo_epi32(x, m2);
            x = _mm256_xor_si256(x, _mm256_srli_epi32(x, 16));
            // x < thr, unsigned: both with the sign bit flipped, signed
            const __m256i lt = _mm256_cmpgt_epi32(t, _mm256_xor_si256(x, sign));
            m |= static_cast<u64>(static_cast<u32>(_mm256_movemask_ps(_mm256_castsi256_ps(lt)))) << l;
            lane = _mm256_add_epi32(lane, eight);
        }
        _mm256_zeroupper();
    }
    for (; l < nl; ++l)
        if (flaw_mix(ks ^ (l * 0x85EBCA6Bu + what * 0xC2B2AE35u)) < thr)
            m |= 1ull << l;
    return m;
}

struct WarpVM {
    typedef u32 uint;
    u32* g_img = nullptr;
    const u32* g_cst = nullptr;
    Lane* g_lanes = nullptr;
    // "groupshared": one group at a time here
    u32 gs_min = 0, gs_count = 0;
    u32 gs_wa[kMaxLanes] = {}, gs_wv[kMaxLanes] = {}, gs_ctl[12] = {};
    u32 g_seen = 0; // the lane being run's template codons read and places searched (Phase 9c)
    u32 g_tseen = 0, g_tfast = 0; // memory tiers (W3): its search's time; its last template's fast codons
    // The step's shared work (warp_isa.inc, w_step_begin): found by the step's
    // first lane that needs it, taken by the rest.
    u32 g_c_skip = 0, g_c_tn = 0, g_c_t0 = 0, g_c_t1 = 0, g_c_tfast = 0, g_c_after = 0, g_c_sdone = 0;
    u32 g_c_to0 = 0, g_c_to1 = 0, g_c_to2 = 0, g_c_seen0 = 0, g_c_seen1 = 0, g_c_seen2 = 0;
    u32 g_c_tseen0 = 0, g_c_tseen1 = 0, g_c_tseen2 = 0;

#define INOUT(T) T&
#define IMG(i) g_img[(i)]
#define CST(i) g_cst[(i)]
#define LANES(i) g_lanes[(i)]
#define ATOMIC_ADD(i, v) (g_img[(i)] += (v))
#define SYNC
#define ONCE_BEGIN {
#define ONCE_END }
#define LANE_BEGIN(l0)                                                                                                 \
    for (u32 l = 0; l < nl; ++l) {                                                                                     \
        Lane& L = LANES((l0) + l);                                                                                     \
        (void)L;
#define LANE_END }
#define LANES_LOAD(l0)
#define LANES_STORE(l0)
#define GS_MIN(x, v)                                                                                                   \
    do {                                                                                                               \
        if ((v) < (x))                                                                                                 \
            (x) = (v);                                                                                                 \
    } while (0)
#define GS_ADD(x, v) ((x) += (v))
#define GS_MAX(x, v)                                                                                                   \
    do {                                                                                                               \
        if ((v) > (x))                                                                                                 \
            (x) = (v);                                                                                                 \
    } while (0)
#define W_LOOP
// (Called only while tracing or counting ops: a call a lane-instruction
// otherwise, for nothing.)
#define W_TRACE(wr, l, step, ip, op, L)                                                                                \
    do {                                                                                                               \
        if (g_ophist || g_trace.on)                                                                                    \
            warp_trace(IMG((wr) + WW_LANE0), (l), (step), (ip), (op), (L), g_seen);                                    \
    } while (0)
#define LANE_T Lane
#define STACK_GET(L, i) ((L).stack[(i)])
#define STACK_SET(L, i, v) ((L).stack[(i)] = (v))
#define WARP_LANE0(wr, wi) IMG((wr) + WW_LANE0)
#define W_FAST_EXEC(b, l0, op, ip, mine, obid, nx, ks) w_fast_exec((b), (l0), (op), (ip), (mine), (obid), (nx), (ks))

#include "warp_isa.inc"

    // A step of a simple instruction as one loop over the lanes (the
    // reference only: warp_isa.inc's W_FAST_EXEC hook). These instructions
    // touch only a lane's registers (an IF moves its IP on one more), so
    // the step's switch is taken once, not once a lane through w_exec, and
    // the lanes' flaw draws come as one mask. Each lane gets exactly what
    // the generic lanes give it: counted, its block set, the instruction,
    // a flaw, its error flags (INERT fails: a fault), its IP. They write
    // nothing, so the step's write phase has nothing to do. False (the
    // generic lanes then run it): another instruction, code not the body's,
    // or a trace or op count being taken (those watch lane by lane).
    bool w_fast_exec(uint b, uint lane0, uint op, uint minip, bool mine, uint obid, uint nx, uint ks) {
        if (!mine || g_ophist || g_trace.on)
            return false;
        switch (op) {
        case OP_NOP0: case OP_NOP1: case OP_NOPX: case OP_INC: case OP_T_INCC: case OP_DEC: case OP_T_DECC:
        case OP_ZERO: case OP_T_ZERO: case OP_ROT: case OP_ROTB: case OP_SWAP: case OP_ADD: case OP_SUB:
        case OP_T_SUBCAB: case OP_T_SUBAAC: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
        case OP_T_NOT0: case OP_SHL: case OP_T_SHL: case OP_SHR: case OP_NEG: case OP_MIN: case OP_MAX:
        case OP_INCA: case OP_T_INCA: case OP_DECA: case OP_INCB: case OP_T_INCB: case OP_DECB: case OP_ZEROA:
        case OP_ZEROB: case OP_T_MOVDC: case OP_T_MOVBA: case OP_YIELD: case OP_IFZ: case OP_T_IFZ: case OP_IFLT:
        case OP_IFGT: case OP_IFNE: case OP_IFEQ: case OP_IFERR: case OP_INERT:
            break;
        default:
            return false;
        }
        const uint nl = CST(WC_LANES);
        const u64 flaws = flaw_mask(ks, nl, IMG(b + WB_FLAW_THR), WD_FLAW);
        const bool fails = op == OP_INERT; // (w_exec's default: not a warp instruction)
        uint skip = W_NONE;               // an IF's: the address after the next instruction
        for (uint l = 0; l < nl; ++l) {
            Lane& L = LANES(lane0 + l);
            if (L.halted != 0u || L.yielded != 0u || L.ip != minip)
                continue;
            GS_ADD(gs_count, 1u);
            L.block = obid;
            bool skips = false;
            switch (op) {
            case OP_INC: case OP_T_INCC: L.cx++; break;
            case OP_DEC: case OP_T_DECC: L.cx--; break;
            case OP_ZERO: case OP_T_ZERO: L.cx = 0u; break;
            case OP_ROT: { const uint x = L.ax, y = L.bx, v = L.cx; L.bx = x; L.cx = y; L.ax = v; break; }
            case OP_ROTB: { const uint x = L.ax, y = L.bx, v = L.cx; L.ax = y; L.bx = v; L.cx = x; break; }
            case OP_SWAP: { const uint x = L.ax; L.ax = L.bx; L.bx = x; break; }
            case OP_ADD: L.cx = L.ax + L.bx; break;
            case OP_SUB: case OP_T_SUBCAB: L.cx = L.ax - L.bx; break;
            case OP_T_SUBAAC: L.ax = L.ax - L.cx; break;
            case OP_MUL: L.cx = L.ax * L.bx; break;
            case OP_AND: L.cx = L.ax & L.bx; break;
            case OP_OR: L.cx = L.ax | L.bx; break;
            case OP_XOR: L.cx = L.ax ^ L.bx; break;
            case OP_NOT: L.cx = ~L.cx; break;
            case OP_T_NOT0: L.cx ^= 1u; break;
            case OP_SHL: case OP_T_SHL: L.cx = L.cx << 1; break;
            case OP_SHR: L.cx = L.cx >> 1; break;
            case OP_NEG: L.cx = 0u - L.cx; break;
            case OP_MIN: L.cx = static_cast<int>(L.ax) < static_cast<int>(L.bx) ? L.ax : L.bx; break;
            case OP_MAX: L.cx = static_cast<int>(L.ax) > static_cast<int>(L.bx) ? L.ax : L.bx; break;
            case OP_INCA: case OP_T_INCA: L.ax++; break;
            case OP_DECA: L.ax--; break;
            case OP_INCB: case OP_T_INCB: L.bx++; break;
            case OP_DECB: L.bx--; break;
            case OP_ZEROA: L.ax = 0u; break;
            case OP_ZEROB: L.bx = 0u; break;
            case OP_T_MOVDC: L.dx = L.cx; break;
            case OP_T_MOVBA: L.bx = L.ax; break;
            case OP_YIELD: L.yielded = 1u; break;
            case OP_IFZ: case OP_T_IFZ: skips = L.cx != 0u; break;
            case OP_IFLT: skips = !(static_cast<int>(L.ax) < static_cast<int>(L.bx)); break;
            case OP_IFGT: skips = !(static_cast<int>(L.ax) > static_cast<int>(L.bx)); break;
            case OP_IFNE: skips = L.ax == L.bx; break;
            case OP_IFEQ: skips = L.ax != L.bx; break;
            case OP_IFERR: skips = L.err == 0u; break;
            default: break; // (NOPs; INERT)
            }
            uint next = nx;
            if (skips) {
                if (skip == W_NONE)
                    skip = w_adv(b, nx, 1u);
                next = skip;
            }
            if ((flaws >> l) & 1u) { // an execution flaw: one register comes out off by one
                const uint fd = w_draw_k(ks, l, WD_FLAW_KIND);
                const uint fdelta = (fd & 1u) != 0u ? 1u : 0xFFFFFFFFu;
                const uint freg = w_below(fd, 3u);
                if (freg == 0u)
                    L.ax += fdelta;
                else if (freg == 1u)
                    L.bx += fdelta;
                else
                    L.cx += fdelta;
                ATOMIC_ADD(b + WB_OUT + WO_FLAWS, 1u);
            }
            if (fails) {
                w_fault(b, L);
            } else {
                L.err = 0u;
                L.errs = 0u;
            }
            L.ip = next;
        }
        return true;
    }

#undef W_FAST_EXEC
#undef WARP_LANE0
#undef LANE_T
#undef STACK_GET
#undef STACK_SET

#undef INOUT
#undef IMG
#undef CST
#undef LANES
#undef ATOMIC_ADD
#undef SYNC
#undef ONCE_BEGIN
#undef ONCE_END
#undef LANE_BEGIN
#undef LANE_END
#undef LANES_LOAD
#undef LANES_STORE
#undef GS_MIN
#undef GS_ADD
#undef GS_MAX
#undef W_LOOP
#undef W_TRACE
};

// The shared source names Ops by number; they must be nova::Op's.
#define NOVA_OP_IS(name, op) static_assert(name == static_cast<u32>(Op::op), #name " is not Op::" #op)
NOVA_OP_IS(OP_NOP0, nop0);
NOVA_OP_IS(OP_NOP1, nop1);
NOVA_OP_IS(OP_JMP, jmp);
NOVA_OP_IS(OP_IFZ, ifz);
NOVA_OP_IS(OP_INC, inc);
NOVA_OP_IS(OP_DEC, dec);
NOVA_OP_IS(OP_ZERO, zero);
NOVA_OP_IS(OP_ROT, rot);
NOVA_OP_IS(OP_PUSH, push);
NOVA_OP_IS(OP_POP, pop);
NOVA_OP_IS(OP_COPY, copy);
NOVA_OP_IS(OP_FIND, find);
NOVA_OP_IS(OP_IFLT, iflt);
NOVA_OP_IS(OP_SENSE, sense);
NOVA_OP_IS(OP_ALLOC, alloc);
NOVA_OP_IS(OP_DIVIDE, divide);
NOVA_OP_IS(OP_CALL, call);
NOVA_OP_IS(OP_RET, ret);
NOVA_OP_IS(OP_DJNZ, djnz);
NOVA_OP_IS(OP_JZ, jz);
NOVA_OP_IS(OP_ADD, add);
NOVA_OP_IS(OP_SUB, sub);
NOVA_OP_IS(OP_MUL, mul);
NOVA_OP_IS(OP_DIV, div);
NOVA_OP_IS(OP_AND, and_);
NOVA_OP_IS(OP_OR, or_);
NOVA_OP_IS(OP_XOR, xor_);
NOVA_OP_IS(OP_NOT, not_);
NOVA_OP_IS(OP_SHL, shl);
NOVA_OP_IS(OP_SHR, shr);
NOVA_OP_IS(OP_NEG, neg);
NOVA_OP_IS(OP_RAND, rand);
NOVA_OP_IS(OP_IFNE, ifne);
NOVA_OP_IS(OP_IFERR, iferr);
NOVA_OP_IS(OP_SWAP, swap);
NOVA_OP_IS(OP_ROTB, rotb);
NOVA_OP_IS(OP_SETH, seth);
NOVA_OP_IS(OP_GETH, geth);
NOVA_OP_IS(OP_READ, read);
NOVA_OP_IS(OP_WRITE, write);
NOVA_OP_IS(OP_ENGULF, engulf);
NOVA_OP_IS(OP_INJECT, inject);
NOVA_OP_IS(OP_EMIT, emit);
NOVA_OP_IS(OP_LISTEN, listen);
NOVA_OP_IS(OP_DISK, disk);
NOVA_OP_IS(OP_GPU, gpu);
NOVA_OP_IS(OP_YIELD, yield);
NOVA_OP_IS(OP_RELEASE, release);
NOVA_OP_IS(OP_T_NOT0, t_not0);
NOVA_OP_IS(OP_T_SHL, t_shl);
NOVA_OP_IS(OP_T_ZERO, t_zero);
NOVA_OP_IS(OP_T_IFZ, t_ifz);
NOVA_OP_IS(OP_T_SUBCAB, t_subcab);
NOVA_OP_IS(OP_T_SUBAAC, t_subaac);
NOVA_OP_IS(OP_T_INCA, t_inca);
NOVA_OP_IS(OP_T_INCB, t_incb);
NOVA_OP_IS(OP_T_DECC, t_decc);
NOVA_OP_IS(OP_T_INCC, t_incc);
NOVA_OP_IS(OP_T_PUSHAX, t_pushax);
NOVA_OP_IS(OP_T_PUSHBX, t_pushbx);
NOVA_OP_IS(OP_T_PUSHCX, t_pushcx);
NOVA_OP_IS(OP_T_PUSHDX, t_pushdx);
NOVA_OP_IS(OP_T_POPAX, t_popax);
NOVA_OP_IS(OP_T_POPBX, t_popbx);
NOVA_OP_IS(OP_T_POPCX, t_popcx);
NOVA_OP_IS(OP_T_POPDX, t_popdx);
NOVA_OP_IS(OP_T_JMPO, t_jmpo);
NOVA_OP_IS(OP_T_JMPB, t_jmpb);
NOVA_OP_IS(OP_T_CALL, t_call);
NOVA_OP_IS(OP_T_RET, t_ret);
NOVA_OP_IS(OP_T_MOVDC, t_movdc);
NOVA_OP_IS(OP_T_MOVBA, t_movba);
NOVA_OP_IS(OP_T_MOVII, t_movii);
NOVA_OP_IS(OP_T_ADRO, t_adro);
NOVA_OP_IS(OP_T_ADRB, t_adrb);
NOVA_OP_IS(OP_T_ADRF, t_adrf);
NOVA_OP_IS(OP_T_MAL, t_mal);
NOVA_OP_IS(OP_T_DIVIDE, t_divide);
NOVA_OP_IS(OP_NOPX, nopx);
NOVA_OP_IS(OP_JMPF, jmpf);
NOVA_OP_IS(OP_JMPB, jmpb);
NOVA_OP_IS(OP_IFEQ, ifeq);
NOVA_OP_IS(OP_IFGT, ifgt);
NOVA_OP_IS(OP_MOD, mod);
NOVA_OP_IS(OP_MIN, min_);
NOVA_OP_IS(OP_MAX, max_);
NOVA_OP_IS(OP_DUP, dup);
NOVA_OP_IS(OP_DROP, drop);
NOVA_OP_IS(OP_INCA, inca);
NOVA_OP_IS(OP_DECA, deca);
NOVA_OP_IS(OP_INCB, incb);
NOVA_OP_IS(OP_DECB, decb);
NOVA_OP_IS(OP_ZEROA, zeroa);
NOVA_OP_IS(OP_ZEROB, zerob);
NOVA_OP_IS(OP_INERT, inert);
NOVA_OP_IS(OP_COUNT, count);
#undef NOVA_OP_IS
static_assert(kSenseReadingsMax <= WB_OUT - WB_SENSE, "the record holds every SENSE reading");
static_assert(WC_HEAD - WC_SENSE >= kSenseReadingsMax, "the constants hold SENSE's whole selector");
static_assert(kMaxLanes <= 64, "lanes fit the groupshared arrays");

// A probability as a 32-bit threshold: an event when a uniform draw is below.
u32 prob_threshold(double p) {
    if (!(p > 0))
        return 0;
    if (p >= 1)
        return 0xFFFFFFFFu;
    const double t = p * 4294967296.0;
    return t >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<u32>(t);
}

} // namespace

// --- warps and lanes ----------------------------------------------------------

u32 Soup::warp_adv(u32 body, u32 a, u32 n) const {
    for (u32 k = bodies_[body].blocks; k != kNone; k = blocks_[k].next) {
        const Block& bl = blocks_[k];
        if (a - bl.start < bl.len)
            return bl.start + (a - bl.start + n % bl.len) % bl.len;
    }
    return (a + n % p_->size) % p_->size;
}

u32 Soup::new_warp(u32 body, u32 ip, const Cpu* launcher) {
    const u32 L = p_->warp_lanes;
    u32 w;
    if (!free_warps_.empty()) {
        w = free_warps_.back();
        free_warps_.pop();
    } else {
        w = static_cast<u32>(warps_.size());
        warps_.push(Warp{});
        lanes_.resize(lanes_.size() + L);
        wleft_.push(0);
        wsteps_.push(0);
        wtime_.push(0);
        wuntil_.push(0);
        wagain_.push(0);
        wlive_.push(0);
        wact_.push(0);
        wstate_.push(kLanesHost);
    }
    Warp& W = warps_[w];
    W = Warp{};
    W.body = body;
    W.next = kNone;
    if (warp_head_[body] == kNone) {
        warp_head_[body] = w;
    } else {
        u32 t = warp_head_[body];
        while (warps_[t].next != kNone)
            t = warps_[t].next;
        warps_[t].next = w;
    }
    ++warp_count_[body];
    const u32 o = owner_[ip];
    const u32 block = o != kNone && blocks_[o].body == body ? o : kNone;
    for (u32 l = 0; l < L; ++l) {
        Lane& x = lanes_[static_cast<usize>(w) * L + l];
        x = Lane{};
        x.ip = ip;
        x.block = block;
        if (launcher) { // a kernel launch with its arguments: the launcher's registers and heads
            x.ax = launcher->ax;
            x.bx = launcher->bx;
            x.cx = launcher->cx;
            x.dx = launcher->dx;
            x.rh = warp_adv(body, launcher->rh, l);
            x.wh = warp_adv(body, launcher->wh, l);
        } else {
            x.rh = x.wh = warp_adv(body, ip, l);
        }
    }
    wleft_[w] = 0;
    wsteps_[w] = 0;
    wtime_[w] = 0;
    wuntil_[w] = 0;
    wagain_[w] = 0;
    wlive_[w] = wact_[w] = L;
    wstate_[w] = kLanesHost; // new lanes: the GPU's copy (if any) is stale
    return w;
}

void Soup::free_warp(u32 w) {
    const u32 b = warps_[w].body;
    if (warp_head_[b] == w) {
        warp_head_[b] = warps_[w].next;
    } else {
        u32 t = warp_head_[b];
        while (warps_[t].next != w)
            t = warps_[t].next;
        warps_[t].next = warps_[w].next;
    }
    --warp_count_[b];
    release_warp_slot(w);
}

// An unlinked warp's slot back to the free list, cleared.
void Soup::release_warp_slot(u32 w) {
    warps_[w] = Warp{};
    warps_[w].body = kNone;
    warps_[w].next = kNone;
    const u32 L = p_->warp_lanes;
    for (u32 l = 0; l < L; ++l)
        lanes_[static_cast<usize>(w) * L + l] = Lane{};
    wlive_[w] = wact_[w] = 0;
    wstate_[w] = kLanesHost;
    wuntil_[w] = 0;
    free_warps_.push(w);
}

void Soup::count_lanes(u32 w) {
    const u32 L = p_->warp_lanes;
    u32 live = 0, act = 0;
    for (u32 l = 0; l < L; ++l) {
        const Lane& x = lanes_[static_cast<usize>(w) * L + l];
        live += x.halted ? 0 : 1;
        act += !x.halted && !x.yielded ? 1 : 0;
    }
    wlive_[w] = live;
    wact_[w] = act;
}

bool Soup::lanes_on_device() const {
    for (usize w = 0; w < wstate_.size(); ++w)
        if (wstate_[w] == kLanesDevice)
            return true;
    return false;
}

void Soup::kill_warps(u32 b) {
    while (warp_head_[b] != kNone)
        free_warp(warp_head_[b]);
    warp_carry_[b] = 0.0f;
    warp_upkeep_[b] = 0.0;
}

// ENGULF: the prey's warps keep running as the eater's, after its own, up to
// warp.max; the rest end.
void Soup::merge_warps(u32 prey, u32 eater) {
    while (warp_head_[prey] != kNone) {
        const u32 w = warp_head_[prey];
        warp_head_[prey] = warps_[w].next;
        --warp_count_[prey];
        warps_[w].next = kNone;
        if (warp_count_[eater] >= p_->warp_max) {
            release_warp_slot(w);
            continue;
        }
        warps_[w].body = eater;
        if (warp_head_[eater] == kNone) {
            warp_head_[eater] = w;
        } else {
            u32 t = warp_head_[eater];
            while (warps_[t].next != kNone)
                t = warps_[t].next;
            warps_[t].next = w;
        }
        ++warp_count_[eater];
    }
    warp_carry_[prey] = 0.0f;
}

u32 Soup::launch_warp(u32 body, u32 ip) {
    if (!p_->warps || warp_count_[body] >= p_->warp_max)
        return kNone;
    return new_warp(body, ip, nullptr);
}

// A new body's runner: a CPU, or a warp for kFormGpu; the other form if it
// holds only that one's units (a CPU when it holds neither: it dies as before).
u32 Soup::birth(u32 body, u32 start, u32 form) {
    const bool has_cpu = bodies_[body].units > 0;
    const bool has_gpu = gpu_count(body) > 0;
    u32 f = form;
    if (!has_cpu && !has_gpu)
        f = kFormCpu;
    else if (f == kFormGpu && !has_gpu)
        f = kFormCpu;
    else if (f == kFormCpu && !has_cpu)
        f = kFormGpu;
    if (f == kFormGpu) {
        new_warp(body, start, nullptr);
        ++tally_.gpu_births;
    } else {
        new_cpu(body, start);
    }
    bodies_[body].form = f;
    return f;
}

bool Soup::cpu_runs(u32 b) const {
    if (bodies_[b].units == 0)
        return false;
    for (u32 c = bodies_[b].cpus; c != kNone; c = cpus_[c].next)
        if (!cpus_[c].halted)
            return true;
    return false;
}

bool Soup::warp_runs(u32 b) const {
    if (gpu_count(b) == 0)
        return false;
    for (u32 w = warp_head_[b]; w != kNone; w = warps_[w].next)
        if (wlive_[w])
            return true;
    return false;
}

void Soup::build_warp_consts() {
    wcst_.resize(WC_COUNT);
    wcst_.fill(0);
    for (u32 i = 0; i < 64; ++i)
        wcst_[WC_TABLE + i] = static_cast<u32>(p_->table.op[i]);
    for (u32 i = 0; i < kSenseReadingsMax; ++i)
        wcst_[WC_SENSE + i] = p_->sel.sense[i];
    wcst_[WC_HEAD + 0] = p_->sel.head[0];
    wcst_[WC_HEAD + 1] = p_->sel.head[1];
    wcst_[WC_LANES] = p_->warp_lanes;
    wcst_[WC_STACK] = p_->stack_depth;
    wcst_[WC_SEARCH] = p_->search_limit;
    wcst_[WC_TEMPLATE] = p_->max_template;
    wcst_[WC_HALT] = p_->halt_after;
    wcst_[WC_SENSE_N] = p_->sel.sense_n;
    wcst_[WC_STEP_COST] = p_->warp_step_cost;
    wcst_[WC_LANE_COST] = p_->warp_lane_cost;
    // Phase 9b: a step's time on a GPU lane, by its instruction (classic: none).
    static_assert(WC_SCAN - WC_OPTIME == kOpCount + 1, "the constants hold a time per Op and one for a fault");
    for (u32 i = 0; i <= kOpCount; ++i)
        wcst_[WC_OPTIME + i] = p_->cost.timed ? p_->cost.gpu_op[i] : 0;
    // Phase 9c: a place searched (classic: searches are free, as in Phase 8).
    wcst_[WC_SCAN] = p_->cost.timed ? static_cast<u32>(p_->cost.gpu_scan) : 0;
}

// --- a round ----------------------------------------------------------------------

bool Soup::warp_pack(u32 round) {
    Lap lap(timing_on_, &timing_.pack);
    wimg_.clear();
    wres_ = nullptr;
    wrec_.clear();
    wbody_.clear();
    wwarp_.clear();
    wrec_gpu_.clear();
    wround_ = round;
    const u32 L = p_->warp_lanes;
    const usize n = sched_n_;
    if (n == 0 || warps_.size() == free_warps_.size())
        return false;
    const bool timed = p_->cost.timed != 0;
    // A warp's clock gives a tick of time a tick (Phase 9b), at the substrate's
    // GPU speed (docs/world-plan.md W1; 1: as before). It reaches the kernel
    // per warp (WW_TLEFT), as its work does (WW_QUOTA, from its units'
    // capacity, which prepare_gpu already scaled).
    const double clock = p_->tick_units * p_->gpu_speed;
    const u32 tick_time = timed ? static_cast<u32>(clock < 4e9 ? clock : 4e9) : 0;
    if (round == 0) { // a new tick: step keys start again (a warp's yields end when it next runs)
        for (u32 w = 0; w < warps_.size(); ++w) {
            if (warps_[w].body == kNone)
                continue;
            wsteps_[w] = 0;
            // Phase 9c: a warp's clock carries what its last step ran past the
            // tick's end (a long search) into this one.
            wtime_[w] = wtime_[w] > tick_time ? wtime_[w] - tick_time : 0;
            wagain_[w] = 0;
        }
        if (!actions_.empty()) // Phase 9b: warps' actions whose end has come land first
            resolve_warp_actions();
    }
    // Phase 9b, timed tables: a warp waiting on a host instruction or an
    // action doesn't run, nor one whose clock is out for this tick.
    auto waits = [&](u32 w) { return timed && (wuntil_[w] > tick_ || wtime_[w] >= tick_time); };
    const bool stamps = !stamp_.empty();
    // SENSE 22/23 for every body at once: the soonest live action on each, in
    // one pass over the actions (action_on scans them all per body: Phase 10
    // profiling found that most of packing's time in a timed world).
    if (timed) {
        if (wact_soon_.size() < bodies_.size())
            wact_soon_.resize(bodies_.size());
        wact_soon_.fill(kNone);
        for (u32 i = 0; i < actions_.size(); ++i) {
            const Action& a = actions_[i];
            if (a.target >= bodies_.size() || a.target_serial != bodies_[a.target].serial || !action_live(a))
                continue;
            u32& best = wact_soon_[a.target];
            if (best == kNone || a.end < actions_[best].end)
                best = i;
        }
    }
    const double serial = static_cast<double>(p_->warp_step_cost) + p_->warp_lane_cost;
    // Memory tiers (docs/world-plan.md W3), timed tables: a lane's prices per
    // tier on this node, for every record. Today's GPU prices -- a body's
    // memory in the L2 (gpu_mem a load, a lane's add a store, gpu_scan a
    // place searched) -- are the slow tier's; the fast tier's are those x
    // gpu_l1 / gpu_mem (the lane's nearest cache). Each / its tier's speed.
    u32 tier_fe = 0, tier_ps[6] = {};
    if (timed && p_->fast_end > 0) {
        tier_fe = p_->fast_end;
        const double near_ratio = static_cast<double>(p_->cost.gpu_l1) / static_cast<double>(p_->cost.gpu_mem);
        const double base[3] = {static_cast<double>(p_->cost.gpu_mem), static_cast<double>(p_->cost.gpu_alu),
                                static_cast<double>(p_->cost.gpu_scan)};
        auto ps = [](double v) { return v >= 4e9 ? 4000000000u : static_cast<u32>(v + 0.5); };
        for (u32 k = 0; k < 3; ++k) {
            tier_ps[2 * k] = ps(base[k] * near_ratio / p_->mem_fast_speed);
            tier_ps[2 * k + 1] = ps(base[k] / p_->mem_slow_speed);
        }
    }
    static_assert(WB_TSTORE == WB_TLOAD + 2 && WB_TSCAN == WB_TLOAD + 4, "the tier prices, in pairs");
    Array<u32>& run = scratch_; // this body's warps to run, then their places in its list
    Array<u32> places;
    for (usize k = 0; k < n; ++k) {
        const u32 b = order_[(sched_start_ + k) % n];
        if (warp_count_[b] == 0 || !bodies_[b].alive) // (the small array first: most bodies have no warp)
            continue;
        run.clear();
        places.clear();
        u32 place = 0;
        for (u32 w = warp_head_[b]; w != kNone; w = warps_[w].next, ++place) {
            // Round 0: any lane not halted (yields end as it starts); later
            // rounds: a lane neither halted nor yielded when it last stopped.
            const bool live = round == 0 ? wlive_[w] > 0 : wact_[w] > 0;
            if (live && !waits(w) && (round == 0 || (wagain_[w] && wleft_[w] > 0))) {
                run.push(w);
                places.push(place);
            }
        }
        if (round == 0) {
            // The body's warp work this tick: what its GPU units have left
            // after its block ops, plus what carried over, less upkeep its
            // CPUs couldn't pay (at the serial rate), split equally among its
            // running warps (the remainder one each from a tick-rotated start).
            // (The capacity holds the substrate's GPU speed, so `units` does.)
            double units = gpu_count(b) ? static_cast<double>(gpu_.cap[b]) / p_->gpu_elements : 0.0;
            if (units < 0)
                units = 0;
            const double work = units * p_->warp_capacity + warp_carry_[b] - warp_upkeep_[b] * serial;
            warp_upkeep_[b] = 0.0;
            if (run.empty()) {
                warp_carry_[b] = work < 0 ? static_cast<float>(work) : 0.0f;
                continue;
            }
            const double whole = work > 0 ? floor(work < 4e9 ? work : 4e9) : 0.0;
            warp_carry_[b] = static_cast<float>(work - whole);
            const u64 total = static_cast<u64>(whole);
            const u64 kk = run.size();
            const u64 q = total / kk, extra = total % kk, off = tick_ % kk;
            for (usize i = 0; i < run.size(); ++i)
                wleft_[run[i]] = static_cast<u32>(q + (((i + kk - off) % kk) < extra ? 1 : 0));
        }
        if (run.empty())
            continue;

        // The record.
        u32 nblk = 0, codons = 0, pages = 0, lo = 0xFFFFFFFFu, hi = 0;
        for (u32 kb = bodies_[b].blocks; kb != kNone; kb = blocks_[kb].next) {
            const Block& bl = blocks_[kb];
            ++nblk;
            codons += bl.len;
            if (bl.len)
                pages += ((bl.start + bl.len - 1) >> kPageBits) - (bl.start >> kPageBits) + 1;
            lo = min_of(lo, bl.start);
            hi = max_of(hi, bl.start + bl.len);
        }
        if (nblk == 0)
            continue;
        // A body of many blocks gets them indexed by start (binary search on
        // the GPU, instead of a scan per address): (start, block) pairs if no
        // two overlap; if some do, the stretches between every block's start
        // and end, each with the block the scan finds there -- the first in
        // the body's list that holds it -- or, between blocks, one that
        // doesn't (any: none does), so a lookup fails as the scan would.
        // (Without it, a body of 500 overlapping blocks scanned them all at
        // every address on the GPU: one held a mature world's round at 130 ms.)
        bool sorted = false;
        u32 npairs = 0;
        if (nblk > 8) {
            Array<u64>& keys = wkeys_;
            Array<u32>& lens = wlens_;
            keys.clear();
            lens.clear();
            u32 j = 0;
            for (u32 kb = bodies_[b].blocks; kb != kNone; kb = blocks_[kb].next, ++j) {
                keys.push((static_cast<u64>(blocks_[kb].start) << 32) | j);
                lens.push(blocks_[kb].len);
            }
            if (keys.size() <= 64) { // insertion sort: the same order, quicker on short, nearly sorted lists
                for (usize i = 1; i < keys.size(); ++i) {
                    const u64 x = keys[i];
                    usize p = i;
                    for (; p > 0 && keys[p - 1] > x; --p)
                        keys[p] = keys[p - 1];
                    keys[p] = x;
                }
            } else {
                sort_u64(keys.data(), keys.size());
            }
            bool disjoint = true;
            u64 prev_end = 0;
            for (usize i = 0; i < keys.size() && disjoint; ++i) {
                const u64 s = keys[i] >> 32;
                disjoint = i == 0 || s >= prev_end;
                prev_end = s + lens[static_cast<u32>(keys[i] & 0xFFFFFFFFu)];
            }
            Array<u32>& seg = wseg_; // (start, block's place in the list) pairs
            seg.clear();
            if (disjoint) {
                for (usize i = 0; i < keys.size(); ++i) {
                    seg.push(static_cast<u32>(keys[i] >> 32));
                    seg.push(static_cast<u32>(keys[i] & 0xFFFFFFFFu));
                }
            } else {
                // Every block's end, sorted, beside the starts (keys); a sweep
                // over the starts and ends, the blocks open at each point in a
                // min-heap by their place in the list (closed ones dropped when
                // they reach its top).
                Array<u64>& ends = wends_;
                Array<u64>& end_of = wendj_; // by place in the list
                Array<u32>& heap = wheap_;
                ends.clear();
                heap.clear();
                end_of.resize(keys.size());
                for (usize i = 0; i < keys.size(); ++i) {
                    const u32 q = static_cast<u32>(keys[i] & 0xFFFFFFFFu);
                    end_of[q] = (keys[i] >> 32) + lens[q];
                    if (lens[q])
                        ends.push(end_of[q] << 32 | q);
                }
                sort_u64(ends.data(), ends.size());
                usize is = 0, ie = 0;
                u32 last = 0xFFFFFFFFu; // the last pair's block (adjacent stretches with one block merge)
                while (ie < ends.size()) {
                    // The next point: the lower of the next start and the next end.
                    u64 x = ends[ie] >> 32;
                    if (is < keys.size() && (keys[is] >> 32) < x)
                        x = keys[is] >> 32;
                    for (; is < keys.size() && (keys[is] >> 32) == x; ++is) {
                        const u32 q = static_cast<u32>(keys[is] & 0xFFFFFFFFu);
                        if (!lens[q])
                            continue;
                        heap.push(q); // (sift up: the smallest place on top)
                        for (usize c = heap.size() - 1; c > 0 && heap[(c - 1) / 2] > heap[c]; c = (c - 1) / 2) {
                            const u32 t = heap[c];
                            heap[c] = heap[(c - 1) / 2];
                            heap[(c - 1) / 2] = t;
                        }
                    }
                    for (; ie < ends.size() && (ends[ie] >> 32) == x; ++ie) {
                    }
                    // Drop closed blocks from the top (ends at or before x).
                    while (!heap.empty() && end_of[heap[0]] <= x) {
                        heap[0] = heap[heap.size() - 1];
                        heap.resize(heap.size() - 1);
                        for (usize c = 0;;) { // (sift down)
                            const usize l = 2 * c + 1, r = l + 1;
                            usize m = c;
                            if (l < heap.size() && heap[l] < heap[m])
                                m = l;
                            if (r < heap.size() && heap[r] < heap[m])
                                m = r;
                            if (m == c)
                                break;
                            const u32 t = heap[c];
                            heap[c] = heap[m];
                            heap[m] = t;
                            c = m;
                        }
                    }
                    const u32 q = heap.empty() ? 0u : heap[0]; // (a gap: block 0 holds none of it)
                    if (q != last || seg.empty()) {
                        seg.push(static_cast<u32>(x));
                        seg.push(q);
                        last = q;
                    }
                }
            }
            sorted = true;
            npairs = static_cast<u32>(seg.size() / 2);
#ifndef NDEBUG
            if (!disjoint) { // the stretches give the scan's answer at every address of the span
                u32 lo_a = 0xFFFFFFFFu, hi_a = 0;
                for (usize i = 0; i < keys.size(); ++i) {
                    const u32 q = static_cast<u32>(keys[i] & 0xFFFFFFFFu);
                    lo_a = min_of(lo_a, static_cast<u32>(keys[i] >> 32));
                    hi_a = max_of(hi_a, static_cast<u32>(wendj_[q]));
                }
                for (u32 a = lo_a > 0 ? lo_a - 1 : 0; a <= hi_a; ++a) {
                    u32 scan = 0xFFFFFFFFu, q = 0;
                    for (u32 kb = bodies_[b].blocks; kb != kNone && scan == 0xFFFFFFFFu; kb = blocks_[kb].next, ++q)
                        if (a - blocks_[kb].start < blocks_[kb].len)
                            scan = q;
                    usize p = 0; // the last stretch starting at or before a
                    for (usize i = 1; i < npairs; ++i)
                        if (seg[2 * i] <= a)
                            p = i;
                    const u32 k = seg[2 * p + 1];
                    const u32 start_k = static_cast<u32>(wendj_[k] - lens[k]);
                    const u32 hit = a - start_k < lens[k] ? k : 0xFFFFFFFFu;
                    NOVA_ASSERT(hit == scan);
                }
            }
#endif
        }
        const u32 rec = static_cast<u32>(wimg_.size());
        const u32 nw = static_cast<u32>(run.size());
        const u32 blk = rec + WB_WARPS + nw * WW_SIZE;
        const u32 code = blk + nblk * WK_SIZE;
        const u32 page = code + codons;
        const u32 stamp = page + pages * WP_SIZE;
        const u32 sort_at = stamp + (stamps ? codons : 0);
        wimg_.resize_unfilled(static_cast<usize>(sort_at) + (sorted ? 1 + 2 * npairs : 0));
        u32* I = wimg_.data();
        // Only the header and the warp entries start as zeros: everything
        // after them is written in full below (Phase 10 profiling: zeroing
        // the whole record was a pass over some 20 MB a tick).
        memset(I + rec, 0, static_cast<usize>(blk - rec) * 4);
        const Body& B = bodies_[b];
        u64 z = Rng::stream_base(seed_ ^ 0x7761727000000000ull, node_, tick_); // "warp"
        z = mix64(z + B.serial);
        const float gerr = gpu_count(b) ? gpu_.err[b] : 1.0f;
        I[rec + WB_NBLK] = nblk;
        I[rec + WB_BLK] = blk;
        I[rec + WB_LO] = lo;
        I[rec + WB_HI] = hi;
        I[rec + WB_SIZE] = p_->size;
        I[rec + WB_KEY] = static_cast<u32>(z ^ (z >> 32));
        I[rec + WB_COPY_THR] = prob_threshold(p_->copy_rate * hazard_mult_ * gerr);
        I[rec + WB_FLAW_THR] = prob_threshold(p_->flaw_rate * hazard_mult_ * gerr);
        if (stamps) {
            const double lim = p_->salvage_ratio * genome_len(b);
            I[rec + WB_SALV_LIM] = lim >= 4e9 ? 4000000000u : static_cast<u32>(lim);
            I[rec + WB_STAMP] = stamp - code;
        }
        I[rec + WB_NWARP] = nw;
        if (tier_fe) { // memory tiers (W3; 0, as zeroed above: one tier)
            I[rec + WB_FAST] = tier_fe;
            for (u32 t = 0; t < 6; ++t)
                I[rec + WB_TLOAD + t] = tier_ps[t];
        }
        if (sorted) { // the pairs' count, then (start, block table entry) pairs
            I[rec + WB_SORT] = sort_at;
            I[sort_at] = npairs;
            for (u32 i = 0; i < npairs; ++i) {
                I[sort_at + 1 + 2 * i] = wseg_[2 * i];
                I[sort_at + 2 + 2 * i] = blk + wseg_[2 * i + 1] * WK_SIZE;
            }
        }
        u32 ok_bits = 0;
        for (u32 r = 0; r < kSenseReadingsMax; ++r) {
            if (r == 0 || r == 4 || r == 20 || r == 21)
                continue; // where each lane runs: the GPU reads those itself
            u32 v = 0;
            if (timed && (r == 22 || r == 23)) { // (as sense_reading, from the pass above)
                ok_bits |= 1u << r;
                I[rec + WB_SENSE + r] = action_reading(r, wact_soon_[b]);
            } else if (sense_reading(b, r, kNone, &v)) {
                ok_bits |= 1u << r;
                I[rec + WB_SENSE + r] = v;
            }
        }
        I[rec + WB_SENSE_OK] = ok_bits;
        for (u32 i = 0; i < nw; ++i) {
            const u32 e = rec + WB_WARPS + i * WW_SIZE;
            I[e + WW_LANE0] = run[i] * L;
            I[e + WW_QUOTA] = wleft_[run[i]];
            I[e + WW_INDEX] = places[i];
            I[e + WW_STEP0] = wsteps_[run[i]];
            I[e + WW_TLEFT] = timed ? tick_time - wtime_[run[i]] : W_NONE; // its own clock (Phase 9b)
        }
        u32 j = 0, cw = code, pe = page;
        for (u32 kb = B.blocks; kb != kNone; kb = blocks_[kb].next, ++j) {
            const Block& bl = blocks_[kb];
            const u32 e = blk + j * WK_SIZE;
            I[e + WK_START] = bl.start;
            I[e + WK_LEN] = bl.len;
            I[e + WK_CODE] = cw;
            I[e + WK_ID] = kb;
            I[e + WK_PAGE] = pe;
            const u32 mine_here = CW_MINE | (j << CW_OWNER_SHIFT);
            // The usual case first, without branches: every codon the block's
            // own. Any that isn't is put right after, one by one.
            const u8* mem = mem_.data() + bl.start;
            const u32* own = owner_.data() + bl.start;
            u32* word = I + cw;
            u32 other = 0;
            for (u32 i = 0; i < bl.len; ++i) {
                word[i] = static_cast<u32>(mem[i]) | mine_here;
                other |= own[i] ^ kb;
            }
            for (u32 i = 0; other && i < bl.len; ++i) {
                const u32 o = own[i];
                if (o == kb)
                    continue;
                if (o != kNone && blocks_[o].body == b) {
                    u32 place_of = 0; // another of the body's blocks: its place in the table
                    for (u32 t = B.blocks; t != o; t = blocks_[t].next)
                        ++place_of;
                    word[i] = static_cast<u32>(mem[i]) | CW_MINE | (place_of << CW_OWNER_SHIFT);
                } else {
                    word[i] = static_cast<u32>(mem[i]) | (0xFFFFu << CW_OWNER_SHIFT);
                }
            }
            if (stamps && bl.len)
                memcpy(I + cw + (stamp - code), stamp_.data() + bl.start, static_cast<usize>(bl.len) * 4);
            cw += bl.len;
            if (bl.len) {
                const u32 first = bl.start >> kPageBits, last = (bl.start + bl.len - 1) >> kPageBits;
                for (u32 pg = first; pg <= last; ++pg, pe += WP_SIZE) {
                    const float h = page_heat_[pg];
                    u32 thr = 0xFFFFFFFFu;
                    if (p_->heat_on && h > p_->mem.hot)
                        thr = prob_threshold(p_->mem.speed(h));
                    I[pe + WP_THR] = thr;
                    I[pe + WP_HEAT] = static_cast<u32>(h);
                    I[pe + WP_FETCH] = 0;
                    I[pe + WP_WRITE] = 0;
                }
            }
        }
        wrec_.push(rec);
        wbody_.push(b);
    }
    wrec_gpu_.resize(wrec_.size()); // none on the GPU until the world says so
    wrec_gpu_.fill(0);
    return !wrec_.empty();
}

void Soup::warp_run_ref() {
    Lap lap(timing_on_, &timing_.ref);
#ifndef NDEBUG
    { // the reference needs the round's lanes here, not only on the GPU
        const u32* I = wimg_.data();
        for (usize r = 0; r < wrec_.size(); ++r)
            for (u32 i = 0; i < I[wrec_[r] + WB_NWARP]; ++i)
                NOVA_ASSERT(wstate_[I[wrec_[r] + WB_WARPS + i * WW_SIZE + WW_LANE0] / p_->warp_lanes] != kLanesDevice);
    }
#endif
    WarpVM vm;
    vm.g_img = wimg_.data();
    vm.g_cst = wcst_.data();
    vm.g_lanes = lanes_.data();
    for (usize i = 0; i < wrec_.size(); ++i)
        vm.w_run_body(wrec_[i]);
}

// One record of the round on the reference (independent of the others: it
// writes only its own record and its own warps' lanes).
void Soup::warp_run_record(usize i) {
    WarpVM vm;
    vm.g_img = wimg_.data();
    vm.g_cst = wcst_.data();
    vm.g_lanes = lanes_.data();
    vm.w_run_body(wrec_[i]);
}

void warp_run_reference(u32* img, const u32* cst, Lane* lanes, u32 rec) {
    WarpVM vm;
    vm.g_img = img;
    vm.g_cst = cst;
    vm.g_lanes = lanes;
    vm.w_run_body(rec);
}

void Soup::warp_unpack(bool all_on_gpu) {
    Lap lap(timing_on_, &timing_.unpack);
    const u32 L = p_->warp_lanes;
    const bool stamps = !stamp_.empty();
    u32 chain = 0;
    if (wround_ == 0 && wbody_chain_.size() < bodies_.size())
        wbody_chain_.resize(bodies_.size());
    for (usize r = 0; r < wrec_.size(); ++r) {
        const u32 rec = wrec_[r], b = wbody_[r];
        const bool on_gpu = all_on_gpu || (r < wrec_gpu_.size() && wrec_gpu_[r]);
        const u32* I = on_gpu && wres_ ? wres_ : wimg_.data(); // (where the round left its words)
        u32 body_steps = 0;
        const u32 nblk = I[rec + WB_NBLK], blk = I[rec + WB_BLK];
        for (u32 j = 0; j < nblk; ++j) {
            const u32 e = blk + j * WK_SIZE;
            const u32 start = I[e + WK_START], len = I[e + WK_LEN], code = I[e + WK_CODE];
            for (u32 i = 0; i < len; ++i) {
                const u32 word = I[code + i];
                if (!(word & CW_WRITTEN))
                    continue;
                const u32 a = start + i;
                mem_[a] = static_cast<u8>(word & CW_CODON);
                if (stamps)
                    stamp_[a] = 0;
                if (obs_)
                    obs_->on_write_gpu(a);
            }
            if (p_->heat_on && len) {
                const u32 first = start >> kPageBits, last = (start + len - 1) >> kPageBits;
                u32 pe = I[e + WK_PAGE];
                for (u32 pg = first; pg <= last; ++pg, pe += WP_SIZE) {
                    page_fetch_[pg] += I[pe + WP_FETCH];
                    page_write_[pg] += I[pe + WP_WRITE];
                }
            }
        }
        tally_.copy_errors += I[rec + WB_OUT + WO_COPYERR];
        tally_.flaws += I[rec + WB_OUT + WO_FLAWS];
        tally_.garbled += I[rec + WB_OUT + WO_GARBLED];
        tally_.halts += I[rec + WB_OUT + WO_HALTS];
        if (p_->endurance > 0)
            writes_ += I[rec + WB_OUT + WO_WRITES];
        const u32 nw = I[rec + WB_NWARP];
        u64 used_all = 0;
        for (u32 i = 0; i < nw; ++i) {
            const u32 e = rec + WB_WARPS + i * WW_SIZE;
            const u32 w = I[e + WW_LANE0] / L;
            const u32 quota = I[e + WW_QUOTA], used = I[e + WW_USED];
            const u32 steps = I[e + WW_STEPS], lins = I[e + WW_LANEINS];
            warps_[w].steps += steps;
            warps_[w].lane_instr += lins;
            wsteps_[w] += steps;
            if (I[rec + WB_FAST]) { // memory tiers (W3): a tiered clock is held at its top, not wrapped
                const u64 t = static_cast<u64>(wtime_[w]) + I[e + WW_TUSED];
                wtime_[w] = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(t);
            } else {
                wtime_[w] += I[e + WW_TUSED]; // Phase 9b (classic: 0)
            }
            tally_.warp_steps += steps;
            tally_.lane_instr += lins;
            if (prof_) // behaviour profiles (A3): observe-only
                prof_->on_lanes(*this, b, lins);
            wleft_[w] = quota > used ? quota - used : 0;
            if (used > quota) // the last step's overshoot is a debt, as a CPU's
                warp_carry_[b] -= static_cast<float>(used - quota);
            warps_[w].wait = I[e + WW_WAIT];
            warps_[w].wait_ip = I[e + WW_WAITIP];
            wlive_[w] = I[e + WW_LIVE];
            wact_[w] = I[e + WW_ACTIVE];
            // Run on the GPU, its lanes are now the GPU's; on the reference, the host's.
            wstate_[w] = on_gpu ? kLanesDevice : kLanesHost;
            used_all += used;
            body_steps += steps;
        }
        chain = body_steps > chain ? body_steps : chain;
        // The split's guess at its next rounds' length (later rounds run a few
        // freed warps): the peak, decaying a quarter a round, since a body
        // whose warps waited on the host one tick runs in full the next.
        if (wround_ == 0) {
            const u32 held = wbody_chain_[b] - wbody_chain_[b] / 4;
            wbody_chain_[b] = body_steps > held ? body_steps : held;
        }
        // Warp work heats the body's GPU units as block ops do: in element-ops.
        if (gpu_count(b) && used_all) {
            const double el = static_cast<double>(used_all) * p_->gpu_elements / p_->warp_capacity;
            const double sum = static_cast<double>(gpu_.done[b]) + el;
            gpu_.done[b] = sum >= 4e9 ? 4000000000u : static_cast<u32>(sum);
        }
    }
    wchain_ = chain;
}

// Phase 9b: at a tick's first round, the warps' actions whose end has come
// (or whose target is gone) land or fail, in the order they began, as the
// host's end of the instruction; the warp then runs on this tick.
void Soup::resolve_warp_actions() {
    for (usize i = 0; i < actions_.size();) {
        const Action& a = actions_[i];
        if (!a.warp || !action_live(a)) {
            ++i;
            continue;
        }
        const bool gone =
            a.target != kNone && (!bodies_[a.target].alive || bodies_[a.target].serial != a.target_serial);
        if (a.end > tick_ && !gone) {
            ++i;
            continue;
        }
        const Action act = a;
        for (usize j = i; j + 1 < actions_.size(); ++j)
            actions_[j] = actions_[j + 1];
        actions_.resize(actions_.size() - 1);
        if (host_step(act.cpu, &act))
            ++tally_.actions_landed;
        else
            ++tally_.actions_failed;
    }
}

void Soup::warp_host(u32 round) {
    Lap lap(timing_on_, &timing_.host);
    (void)round;
    const u32 L = p_->warp_lanes;
    Array<u32> waiting; // in the schedule's order, then each body's
    for (usize r = 0; r < wrec_.size(); ++r) {
        const u32 rec = wrec_[r];
        const u32* I = warp_round_image(r);
        const u32 nw = I[rec + WB_NWARP];
        for (u32 i = 0; i < nw; ++i) {
            const u32 w = I[rec + WB_WARPS + i * WW_SIZE + WW_LANE0] / L;
            wagain_[w] = 0;
            if (I[rec + WB_WARPS + i * WW_SIZE + WW_WAIT])
                waiting.push(w);
        }
    }
    for (usize i = 0; i < waiting.size(); ++i) {
        const u32 w = waiting[i];
        // An earlier host instruction may have ended this warp or moved it
        // (ENGULF): it runs for whoever holds it now, or not at all.
        if (warps_[w].body == kNone || !warps_[w].wait)
            continue;
        host_stalled_ = false;
        host_step(w);
        wagain_[w] = host_stalled_ ? 0 : 1; // (gestation, G4: a waiting DIVIDE doesn't run again this tick)
    }
    for (usize i = 0; i < waiting.size(); ++i) // a host instruction not done is retried next tick
        warps_[waiting[i]].wait = 0;
}

} // namespace nova
