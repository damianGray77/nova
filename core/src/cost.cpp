#include "nova/cost.h"

#include <cstdio>
#include <cstring>

namespace nova {

namespace {

// Each instruction's own part, as the machine's own code would be: an add for
// most, a multiply, a divide, a load, a store, a random draw, a cache line
// between cores. What depends on how it runs (template codons read, codons
// searched, a jump taken, a kernel call, codons moved, the disk) is added by
// the interpreter as it runs (interp.cpp).
void recipes(CostTable* t) {
    for (u32 i = 0; i < kOpCount; ++i)
        t->op[i] = t->alu;
    auto set = [t](Op o, u32 v) { t->op[static_cast<u32>(o)] = v; };
    set(Op::mul, t->mul);
    set(Op::div, t->div);
    set(Op::mod, t->div);
    // the stack: a store or a load
    set(Op::push, t->store);
    set(Op::dup, t->store);
    set(Op::t_pushax, t->store);
    set(Op::t_pushbx, t->store);
    set(Op::t_pushcx, t->store);
    set(Op::t_pushdx, t->store);
    set(Op::call, t->store); // the return address (and a jump, when it finds its target)
    set(Op::t_call, t->store);
    set(Op::pop, t->load);
    set(Op::drop, t->load);
    set(Op::t_popax, t->load);
    set(Op::t_popbx, t->load);
    set(Op::t_popcx, t->load);
    set(Op::t_popdx, t->load);
    set(Op::ret, t->load); // (and a jump)
    set(Op::t_ret, t->load);
    // memory through the heads
    set(Op::copy, t->load + t->store);
    set(Op::t_movii, t->load + t->store);
    set(Op::read, t->load);
    set(Op::write, t->store);
    // readings and chance
    set(Op::sense, t->load);
    set(Op::rand, t->draw);
    // the signal channel: shared between every body on the node
    set(Op::emit, t->share);
    set(Op::listen, t->share);

    // Phase 9b: a warp step on the GPU, by its instruction. A lane's add for
    // most; a multiply, a divide (emulated on a GPU: 22 adds); the stack in
    // the lane's nearest cache; the body's memory (a megabyte or so, in the
    // GPU's L2) for COPY, READ and SENSE; a hash of adds for RAND. A step's
    // template searches come on top (Phase 9c): the slowest lane's places
    // searched and template codons read, at gpu_scan each (warp_isa.inc).
    const u32 a = static_cast<u32>(t->gpu_alu), l1 = static_cast<u32>(t->gpu_l1),
              mem = static_cast<u32>(t->gpu_mem);
    for (u32 i = 0; i <= kOpCount; ++i)
        t->gpu_op[i] = a;
    auto gset = [t](Op o, u32 v) { t->gpu_op[static_cast<u32>(o)] = v; };
    gset(Op::mul, static_cast<u32>(t->gpu_mul));
    gset(Op::div, static_cast<u32>(t->gpu_div));
    gset(Op::mod, static_cast<u32>(t->gpu_div));
    const Op stack[] = {Op::push,     Op::pop,      Op::dup,      Op::drop,     Op::call,     Op::ret,
                        Op::t_pushax, Op::t_pushbx, Op::t_pushcx, Op::t_pushdx, Op::t_popax,  Op::t_popbx,
                        Op::t_popcx,  Op::t_popdx,  Op::t_call,   Op::t_ret};
    for (Op o : stack)
        gset(o, l1);
    gset(Op::copy, mem + a);
    gset(Op::t_movii, mem + a);
    gset(Op::read, mem);
    gset(Op::sense, mem);
    gset(Op::rand, 8 * a);
}

const u32 kHomeV1[] = {
    // Damian's home rig, 2026-11-06 (`nova bench costs`, medians of four runs
    // (mul: of three, after its kernel was fixed), a P-core of the Core Ultra 9
    // 285K at about 5.3 GHz; the disk on E:).
    190,      // alu
    578,      // mul
    2650,     // div
    755,      // load
    95,       // store
    4560,     // jump
    275,      // scan
    16,       // move
    2650,     // draw
    389000,   // call      (a kernel round trip, 389 ns)
    40000,    // share     (a cache line between P-cores, 40 ns)
    45600000, // disk_read (a random 4 KB read, 45.6 us)
    12300000, // disk_write (a random 4 KB write, 12.3 us)
    241,      // disk_read_codon  (4.1 GB/s in sequence)
    200,      // disk_write_codon (5.0 GB/s)
    3600,     // load_far (memory tiers, docs/world-plan.md W3): a load from 1 MB, 3.6 ns, as the
              // same runs measured it (design.md 6.15: 1.0 ns at 64 KB, 3.6 ns at 1 MB, ...)
};

// Memory tiers (W3): the E-core's load from 1 MB wasn't recorded, so it is
// its own nearest-cache load scaled as the P-core's is (872 x 3600 / 755 =
// 4157.9): scaled, not measured.
const u32 kHomeV1ELoadFar = 4158;

const u64 kHomeV1Dev[] = {
    // Phase 9b: the RTX 5080 (a lane on its own clock, medians of 5) and the
    // Intel AI Boost NPU, 2026-11-06, the means of two runs of `nova bench
    // costs`.
    1393,       // gpu_alu
    2028,       // gpu_mul
    30900,      // gpu_div    (emulated)
    18800,      // gpu_l1     (a 4 KB chase)
    145700,     // gpu_mem    (a 1 MB chase: the L2)
    368900,     // gpu_vram   (a 256 MB chase)
    700,        // gpu_thr    (1.54e13 lane-ops a second over 10,752 lanes)
    67000000,   // gpu_start  (a lone op's round trip, 67 us)
    47000,      // gpu_share  (each further op in a batch of 64, 47 ns)
    3370000000, // npu_start  (3.37 ms)
    10300000,   // npu_share  (10.3 us)
    2200,       // npu_mac    (0.45 G a second, through DirectML's float16 graphs)
    32700,      // gpu_scan   (Phase 9c: a lane's search loop, a place, the same in two runs)
};

const u32 kHomeV1E[] = {
    // Phase 9c: the same rig's E-cores (`nova bench costs`, the means of two
    // runs, 2026-11-06; an E-core of the 285K at about 4.6 GHz). alu .. call;
    // share and the disk are the P-core's. Its bulk move is the one large gap
    // (92 against 16, in both runs).
    218,    // alu
    655,    // mul
    4425,   // div
    872,    // load
    109,    // store
    4996,   // jump
    437,    // scan
    92,     // move
    3489,   // draw
    631000, // call (631 ns)
};

void put(TextBuf& out, const char* what, u32 ps, u32 alu, u32 eps) {
    char line[112];
    if (eps)
        snprintf(line, sizeof line, "  %-18s %12u ps %14.1f adds %12u ps on an E-core\n", what, ps,
                 alu ? static_cast<double>(ps) / alu : 0.0, eps);
    else
        snprintf(line, sizeof line, "  %-18s %12u ps %14.1f adds\n", what, ps, alu ? static_cast<double>(ps) / alu : 0.0);
    out.append(line);
}

} // namespace

bool builtin_costs(Str name, CostTable* out) {
    *out = CostTable{};
    if (name.eq("classic")) {
        memcpy(out->name, "classic", 8);
        out->timed = 0;
        for (u32 i = 0; i < kOpCount; ++i)
            out->op[i] = 1;
        return true;
    }
    if (name.eq("home_v1")) {
        memcpy(out->name, "home_v1", 8);
        out->timed = 1;
        u32* f = &out->alu;
        static_assert(sizeof kHomeV1 / sizeof kHomeV1[0] == kCostCpuOps, "home_v1: the CPU's operations");
        for (u32 i = 0; i < kCostCpuOps; ++i)
            f[i] = kHomeV1[i];
        u64* d = &out->gpu_alu;
        static_assert(sizeof kHomeV1Dev / sizeof kHomeV1Dev[0] == kCostDevOps, "home_v1: the devices' operations");
        for (u32 i = 0; i < kCostDevOps; ++i)
            d[i] = kHomeV1Dev[i];
        recipes(out);
        return true;
    }
    return false;
}

bool builtin_costs_e(Str name, CostTable* out) {
    if (!builtin_costs(name, out))
        return false;
    if (name.eq("home_v1")) {
        u32* f = &out->alu;
        static_assert(sizeof kHomeV1E / sizeof kHomeV1E[0] == 10, "home_v1's E-core: alu .. call");
        for (u32 i = 0; i < 10; ++i)
            f[i] = kHomeV1E[i];
        out->load_far = kHomeV1ELoadFar; // memory tiers (W3)
        recipes(out);
    }
    return true;
}

void describe_costs(const CostTable& t, TextBuf& out) {
    out.append("cost table ");
    out.append(t.name);
    if (!t.timed) {
        out.append(": classic, every instruction costs 1 (and the old formulas for contact, signals, the disk "
                   "and the accelerators)\n");
        return;
    }
    out.append(": world time from the machine's own operations\n\nthe operations:\n");
    const char* const names[] = {"alu (an add)", "mul",        "div",       "load",           "store",
                                 "jump",         "scan",       "move",      "draw",           "call",
                                 "share",        "disk_read",  "disk_write", "disk_read_codon", "disk_write_codon",
                                 "load_far (1 MB)"};
    static_assert(sizeof names / sizeof names[0] == kCostCpuOps, "a name for each CPU operation");
    CostTable e;
    if (!builtin_costs_e(Str(t.name), &e))
        e = t;
    const u32* f = &t.alu;
    const u32* fe = &e.alu;
    for (u32 i = 0; i < kCostCpuOps; ++i) // (alu .. call and load_far have an E-core's own)
        put(out, names[i], f[i], t.alu, i < 10 || i == kCostCpuOps - 1 ? fe[i] : 0);
    const char* const dnames[] = {"gpu_alu",   "gpu_mul",   "gpu_div",   "gpu_l1",    "gpu_mem",
                                  "gpu_vram",  "gpu_thr",   "gpu_start", "gpu_share", "npu_start",
                                  "npu_share", "npu_mac",   "gpu_scan"};
    const u64* d = &t.gpu_alu;
    for (u32 i = 0; i < kCostDevOps; ++i) {
        char line[96];
        snprintf(line, sizeof line, "  %-18s %12llu ps %14.1f adds\n", dnames[i], static_cast<unsigned long long>(d[i]),
                 t.alu ? static_cast<double>(d[i]) / t.alu : 0.0);
        out.append(line);
    }
    out.append("\neach instruction's own part (searches, jumps, kernel calls, codons moved and the disk come on "
               "top as it runs), on an E-core, and a warp step's time on the GPU:\n");
    for (u32 i = 0; i < kOpCount; ++i) {
        char line[144];
        snprintf(line, sizeof line, "  %-18s %12u ps %14.1f adds %12u ps on an E-core %12u ps on a GPU lane\n",
                 op_name(static_cast<Op>(i)), t.op[i], t.alu ? static_cast<double>(t.op[i]) / t.alu : 0.0, e.op[i],
                 t.gpu_op[i]);
        out.append(line);
    }
}

} // namespace nova
