#pragma once

// Asynchronous accelerator ops (Phase 5, design.md 6.11).
//
// With accel.mode = async an accelerator op is checked, sized, charged and
// queued when its CPU issues it; nothing is read or written then. At the
// barrier the world gathers every node's queued ops (reading their inputs from
// memory as it is at that barrier), runs them as one batch on the GPU, the NPU
// or the CPU reference, and writes the results back at the barrier
// accel.latency - 1 ticks later. Everything is integer arithmetic, so every
// device gives the same answer: the CPU reference below is the definition.
#include "nova/types.h"

namespace nova {

// Op kinds: the accelerator verb's BX after its selector map. 0-15 GPU, 16-19 NPU.
enum AccelKind : u32 {
    kAccCompare, kAccCount, kAccFind, kAccSum,                       // reductions -> scalar
    kAccCopy, kAccFill, kAccNot, kAccRcopy,                          // elementwise -> codons
    kAccAdd, kAccSub, kAccXor, kAccAnd, kAccOr, kAccMin, kAccMax, kAccMul,
    kAccDot, kAccMatvec, kAccMatmul, kAccArgmax                      // NPU
};

inline bool accel_is_npu(u32 kind) { return kind >= kAccDot; }
// Does the op leave a scalar result (into the CPU's result slot)?
inline bool accel_has_scalar(u32 kind) {
    return kind <= kAccSum || kind == kAccDot || kind == kAccArgmax;
}
// "Not found" for find.
inline constexpr u32 kAccNotFound = 0xFFFFFFFFu;

// One queued op. Plain data: hashed and snapshotted field by field.
struct AccelOp {
    u64 due;        // completes at the barrier ending this tick
    u64 serial;     // the issuing node's op count (its stream key)
    u64 body_serial; // the issuing body, by creation serial (slots are reused)
    u32 node;
    u32 kind;       // AccelKind
    u32 body, cpu, cpu_gen; // where the results go
    u32 n;          // GPU: elements; NPU: k
    u32 key;        // AX & 63 (count, find, fill)
    u32 genome;     // the issuer's genome length (the salvage rule)
    u32 in_off;     // inputs, in the batch's input codons: src, then dst, then the matrix
    u32 src_n, dst_n, mat_n;
    u32 addr_off;   // destination addresses (for writing back), in the batch's addresses
    u32 out_n;      // output codons
    float gerr;     // copy-error multiplier at issue (GPU heat)
    u32 pad;
};
static_assert(sizeof(AccelOp) == 3 * 8 + 16 * 4, "AccelOp must have no implicit padding");

// Output codons an op of this kind and size writes back.
u32 accel_out_codons(u32 kind, u32 n);

// The CPU reference: computes one op from its inputs. `in` holds src_n source
// codons, then dst_n destination codons, then mat_n matrix codons. Writes
// out_n codons to out and returns the scalar result (0 if none). Copy errors
// and the salvage rule are applied by the soup, not here: this is pure.
u32 accel_reference(const AccelOp& op, const u8* in, u8* out);

} // namespace nova
