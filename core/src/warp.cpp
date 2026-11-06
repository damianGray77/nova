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
#include <cstring>

#include "nova/lineage.h"
#include "nova/soup.h"

#include "warp_layout.inc"

namespace nova {

// --- the interpreter, compiled as C++ --------------------------------------------

namespace {

struct WarpVM {
    typedef u32 uint;
    u32* g_img = nullptr;
    const u32* g_cst = nullptr;
    Lane* g_lanes = nullptr;
    // "groupshared": one group at a time here
    u32 gs_min = 0, gs_count = 0;
    u32 gs_wa[kMaxLanes] = {}, gs_wv[kMaxLanes] = {}, gs_ctl[12] = {};
    u32 g_seen = 0; // the lane being run's template codons read and places searched (Phase 9c)

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
#define LANE_T Lane
#define STACK_GET(L, i) ((L).stack[(i)])
#define STACK_SET(L, i, v) ((L).stack[(i)] = (v))
#define WARP_LANE0(wr, wi) IMG((wr) + WW_LANE0)

#include "warp_isa.inc"

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
    wimg_.clear();
    wrec_.clear();
    wbody_.clear();
    wwarp_.clear();
    const u32 L = p_->warp_lanes;
    const usize n = sched_n_;
    if (n == 0 || warps_.size() == free_warps_.size())
        return false;
    const bool timed = p_->cost.timed != 0;
    const u32 tick_time = timed ? static_cast<u32>(p_->tick_units < 4e9 ? p_->tick_units : 4e9) : 0;
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
    const double serial = static_cast<double>(p_->warp_step_cost) + p_->warp_lane_cost;
    Array<u32>& run = scratch_; // this body's warps to run, then their places in its list
    Array<u32> places;
    for (usize k = 0; k < n; ++k) {
        const u32 b = order_[(sched_start_ + k) % n];
        if (!bodies_[b].alive || warp_count_[b] == 0)
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
        // the GPU, instead of a scan per address), if no two overlap.
        bool sorted = false;
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
            sort_u64(keys.data(), keys.size());
            sorted = true;
            u64 prev_end = 0;
            for (usize i = 0; i < keys.size() && sorted; ++i) {
                const u64 s = keys[i] >> 32;
                sorted = i == 0 || s >= prev_end;
                prev_end = s + lens[static_cast<u32>(keys[i] & 0xFFFFFFFFu)];
            }
        }
        const u32 rec = static_cast<u32>(wimg_.size());
        const u32 nw = static_cast<u32>(run.size());
        const u32 blk = rec + WB_WARPS + nw * WW_SIZE;
        const u32 code = blk + nblk * WK_SIZE;
        const u32 page = code + codons;
        const u32 stamp = page + pages * WP_SIZE;
        const u32 sort_at = stamp + (stamps ? codons : 0);
        wimg_.resize(static_cast<usize>(sort_at) + (sorted ? 2 * nblk : 0));
        u32* I = wimg_.data();
        for (u32 i = rec; i < wimg_.size(); ++i)
            I[i] = 0;
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
        if (sorted) {
            I[rec + WB_SORT] = sort_at;
            for (u32 i = 0; i < nblk; ++i) {
                I[sort_at + 2 * i] = static_cast<u32>(wkeys_[i] >> 32);
                I[sort_at + 2 * i + 1] = blk + static_cast<u32>(wkeys_[i] & 0xFFFFFFFFu) * WK_SIZE;
            }
        }
        u32 ok_bits = 0;
        for (u32 r = 0; r < kSenseReadingsMax; ++r) {
            if (r == 0 || r == 4 || r == 20 || r == 21)
                continue; // where each lane runs: the GPU reads those itself
            u32 v = 0;
            if (sense_reading(b, r, kNone, &v)) {
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
            for (u32 i = 0; i < bl.len; ++i) {
                const u32 a = bl.start + i;
                const u32 o = owner_[a];
                u32 word = mem_[a];
                if (o == kb) { // the usual case: this block's own codon
                    word |= mine_here;
                } else if (o != kNone && blocks_[o].body == b) {
                    u32 place_of = 0; // another of the body's blocks: its place in the table
                    for (u32 t = B.blocks; t != o; t = blocks_[t].next)
                        ++place_of;
                    word |= CW_MINE | (place_of << CW_OWNER_SHIFT);
                } else {
                    word |= 0xFFFFu << CW_OWNER_SHIFT;
                }
                I[cw + i] = word;
                if (stamps)
                    I[cw + (stamp - code) + i] = stamp_[a];
            }
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
                }
            }
        }
        wrec_.push(rec);
        wbody_.push(b);
    }
    return !wrec_.empty();
}

void Soup::warp_run_ref() {
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

void Soup::warp_unpack(bool on_gpu) {
    const u32 L = p_->warp_lanes;
    const u32* I = wimg_.data();
    const bool stamps = !stamp_.empty();
    u32 chain = 0;
    for (usize r = 0; r < wrec_.size(); ++r) {
        const u32 rec = wrec_[r], b = wbody_[r];
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
            wtime_[w] += I[e + WW_TUSED]; // Phase 9b (classic: 0)
            tally_.warp_steps += steps;
            tally_.lane_instr += lins;
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
    (void)round;
    const u32* I = wimg_.data();
    const u32 L = p_->warp_lanes;
    Array<u32> waiting; // in the schedule's order, then each body's
    for (usize r = 0; r < wrec_.size(); ++r) {
        const u32 rec = wrec_[r];
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
        host_step(w);
        wagain_[w] = 1;
    }
    for (usize i = 0; i < waiting.size(); ++i) // a host instruction not done is retried next tick
        warps_[waiting[i]].wait = 0;
}

} // namespace nova
