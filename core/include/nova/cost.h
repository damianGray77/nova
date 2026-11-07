#pragma once

// Measured costs (Phase 9, design.md 6.15, docs/phase9-plan.md): what each
// instruction costs. Two built-in tables:
//
//   classic  every instruction costs 1, with the old formulas for contact,
//            signals, the disk and the accelerators: exactly Phase 8, every
//            hash unchanged.
//   home_v1  world time in picoseconds, from this machine's own basic
//            operations, measured once outside the simulation by `nova bench
//            costs` (Damian's home rig, 2026-11-06: an Intel Core Ultra 9
//            285K P-core, the disk on E:). Each instruction costs what it
//            would as the machine's own code: its own part (an add, a
//            multiply, a load ...) plus, as it runs, the template codons it
//            reads, the codons it searches, a jump it takes, a kernel call
//            for a world change, the codons it moves, and so on.
//
// Runs never measure: a table is a fixed set of whole numbers, so every run,
// replay and snapshot uses exactly the same costs. A re-measure is a new
// table (home_v2), never a silent change. ENGULF's and INJECT's pace are
// world rules, not measurements (engulf.codons_per_tick and
// inject.codons_per_tick, SoupParams).
#include "nova/isa.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

struct CostTable {
    char name[16];
    u32 timed; // 0: classic (instructions); 1: picoseconds of world time
    // The machine's basic operations, picoseconds (timed tables only):
    u32 alu;   // an add
    u32 mul;   // a multiply
    u32 div;   // a 32-bit divide
    u32 load;  // a load from the nearest cache
    u32 store; // a store
    u32 jump;  // a jump the hardware can't predict, over one it can
    u32 scan;  // a codon searched (template searches, contact's survey)
    u32 move;  // a codon moved in bulk (its byte and its owner)
    u32 draw;  // a random draw
    u32 call;  // a kernel call: the price of every world change
    u32 share; // a cache line passed between cores: the signal channel
    u32 disk_read, disk_write;             // a random disk access
    u32 disk_read_codon, disk_write_codon; // a codon read or written in sequence
    // Memory tiers (docs/world-plan.md W3): a load from 1 MB, one cache level
    // out from `load` -- the slow tier's load; its other operations are scaled
    // by load_far / load (interp.cpp, Exec::tiers_setup).
    u32 load_far;
    // Each instruction's own part (the rest is added as it runs); classic: 1.
    u32 op[kOpCount];
    // Phase 9b, the devices, picoseconds:
    u64 gpu_alu, gpu_mul, gpu_div; // a GPU lane's dependent add, multiply, divide
    u64 gpu_l1, gpu_mem, gpu_vram; // ... load: its nearest cache, 1 MB (a body's memory), video memory
    u64 gpu_thr;                   // a lane-op's share of one lane, the whole GPU busy
    u64 gpu_start, gpu_share;      // a lone GPU op's round trip; what each further op in a batch adds
    u64 npu_start, npu_share;      // the same on the NPU
    u64 npu_mac;                   // a multiply-add, the whole NPU
    u64 gpu_scan;                  // a place a GPU lane's template search looks at (Phase 9c)
    // A warp step's time by its instruction, on the GPU (the last: a lane
    // that can't run the code); classic: 0.
    u32 gpu_op[kOpCount + 1];
};
inline constexpr u32 kCostCpuOps = 16; // alu .. disk_write_codon, load_far (W3)
inline constexpr u32 kCostDevOps = 13; // gpu_alu .. gpu_scan

// Built-in tables by name ("classic", "home_v1"); false for an unknown name.
bool builtin_costs(Str name, CostTable* out);
// Phase 9c: the same table on an E-core (design.md 6.15): its CPU operations
// (alu .. call) as an E-core runs them; the devices, the disk and a cache
// line between cores as on the P-core. Classic: the classic table.
bool builtin_costs_e(Str name, CostTable* out);

// For `nova costs`: the table, each operation and each instruction's own part
// in picoseconds and in adds, on a P-core and an E-core.
void describe_costs(const CostTable& t, TextBuf& out);

} // namespace nova
