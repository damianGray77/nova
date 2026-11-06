#pragma once

// The real accelerators (Phase 5, design.md 6.11): the machine's GPUs and NPU,
// reached through Windows' own Direct3D 12, DXCore and DirectML. Everything
// Direct3D lives in accel_dx.cpp; nothing else includes its headers.
//
// This file has device discovery and the probes `nova devices` runs: a
// known-answer compute shader on each GPU, and known-answer matrix multiplies
// through DirectML (int8 -> int32, float32, float16) on each device that can
// run it, so we learn what each device accepts and whether its answers are
// exact.
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

struct AccelDevice {
    char name[128];
    u64 memory;     // dedicated memory, bytes
    u32 index;      // position in accel_list's order
    u8 gpu;         // a GPU (Direct3D 12 graphics and compute)
    u8 npu;         // an NPU (DirectML only: the generic ML feature level)
    u8 hardware;    // not a software adapter
    u8 integrated;  // shares system memory
};

// Lists the adapters that can run Direct3D 12 compute or DirectML: GPUs first,
// most dedicated memory first, then NPUs and other compute accelerators.
// Returns how many (at most max).
u32 accel_list(AccelDevice* out, u32 max);

// Runs a compute shader on device `index` and checks every value it wrote.
bool accel_probe_compute(u32 index, TextBuf* report);

// Phase 9 (`nova bench costs`, design.md 6.15): the GPU's own operations,
// timed on its own clock with native microbenchmarks. Picoseconds a lane's
// dependent op (one 32-lane group, each lane its own chain), and the lane-ops
// a second of the whole GPU kept busy. Each is the median of 5 repeats.
struct GpuMicro {
    double alu, mul, div;  // an add, a multiply, a 32-bit divide
    double l1, mem, vram;  // a load: from 4 KB, 1 MB and 256 MB of video memory
    double ops_per_s;      // independent adds a second, every lane busy
    double scan;           // a place searched (Phase 9c): a lane's search loop through 4 KB
};
bool gpu_microbench(u32 index, GpuMicro* out, TextBuf* err);

// The matrix multiply forms DirectML is asked for, by bit.
enum : u32 {
    kProbeInt8 = 1,         // int8 x int8 -> int32
    kProbeFloat32 = 2,      // exact for in-world sizes (sums below 2^24)
    kProbeFloat16 = 4,      // full range: never exact (sums overflow float16)
    kProbeFloat16Small = 8, // float16 with every sum below 2048: exact
    kProbeGraph = 16,       // ran as a compiled one-node graph
    kProbeWeights = 32,     // with B handed over as fixed weights
    kProbeConv = 64         // as a 1x1 convolution
};

// Known-answer matrix multiplies through DirectML on device `index`, with the
// value range and sizes in-world NPU ops use. Returns the forms that ran and
// gave exactly the right answer (kProbe... bits); report says what happened
// with each.
u32 accel_probe_dml(u32 index, TextBuf* report);

// Does device `index` compile (and initialize) the float16 multiply
// [batch, m x 32] x [batch, 32 x n], and in how many milliseconds? layout:
// where the batch goes (0: [batch, 1, m, k], 1: [1, batch, m, k], 2: three
// dimensions).
bool accel_probe_shape(u32 index, u32 batch, u32 m, u32 n, u32 layout, double* ms);

// --- the backends -------------------------------------------------------------

struct AccelOp;

// The GPU backend: one Direct3D 12 compute shader that does every op kind
// (all 16 GPU ops and the 4 NPU ops), exactly as the CPU reference does. A
// batch is submitted at one barrier and collected at a later one; up to
// kAccelSlots batches can be in flight.
inline constexpr u32 kAccelSlots = 8;

class GpuBackend {
  public:
    GpuBackend() = default;
    ~GpuBackend() { close(); }
    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;

    // Opens device `index` (accel_list order) and compiles the kernel.
    bool open(u32 index, TextBuf* err);
    void close();
    bool is_open() const { return impl_ != nullptr; }
    const char* name() const;

    // Starts ops[pick[0..n)] (inputs in `in` at each op's in_off). Returns a
    // ticket for collect, or -1 (no free slot, or a device error: the caller
    // falls back to the CPU reference).
    int submit(const AccelOp* ops, const u32* pick, u32 n, const u8* in);
    // Waits for a ticket's batch: each op's output codons, op after op (out_n
    // each), into codons; each op's scalar into scalars. False on a device
    // error.
    bool collect(int ticket, u8* codons, u32* scalars);

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Warps on the GPU (Phase 8, design.md 6.14): warp_isa.inc compiled as HLSL
// (the very source the CPU reference runs), a thread group per body and a
// thread per lane. A round takes every node's packed image and lanes (soup.h
// warp_pack), runs every record, and hands back the written image and lanes,
// bit for bit what the reference gives.
struct WarpJob {
    u32 node;            // which node: where its lanes live on the GPU
    u32* img;            // the node's image (warp_isa.inc's records): read and written
    u32 img_words;
    const u32* recs;     // its record offsets
    u32 nrec;
    const void* lanes;   // the node's lanes on the host (soup.h Lane), by slot
    u32 nlanes;          // how many (warp slots x lanes)
    const u32* send;     // warp slots whose host lanes must go to the GPU first
    u32 nsend;
};

// A warp's lanes to bring back from the GPU into the host's copy.
struct WarpFetch {
    u32 node, slot;
    void* dst; // the host's lanes of that warp (lanes-per-warp Lanes)
};

// What the warp rounds moved and where their time went (for --profile).
struct WarpGpuStats {
    u64 rounds = 0, records = 0;
    u64 bytes_up = 0, bytes_down = 0;
    u64 lanes_sent = 0, lanes_fetched = 0; // warps' lanes moved (each warp's, lanes-per-warp Lanes)
    double ms_fetch = 0;                   // fetching lanes back (a round trip each time)
    double ms_in = 0;   // copying the round into the upload buffer (CPU)
    double ms_gpu = 0;  // the device: upload, kernel, download
    double ms_out = 0;  // copying the results out of the readback buffer (CPU)
    // the device's own clock (timestamps), when it has one
    double ms_dev_up = 0, ms_dev_kernel = 0, ms_dev_down = 0;
    // warp steps run: in all, and the longest body's (summed over rounds)
    u64 steps = 0, steps_longest = 0;
    u64 img_words = 0, rec_words_max = 0; // image words moved; the largest body record seen
};

class WarpGpu {
  public:
    const WarpGpuStats& stats() const { return stats_; }
    WarpGpu() = default;
    ~WarpGpu() { close(); }
    WarpGpu(const WarpGpu&) = delete;
    WarpGpu& operator=(const WarpGpu&) = delete;

    // Opens device `index` (accel_list order) and compiles the kernel for
    // `lanes` lanes a warp; compile_ms (if given) gets the compile time.
    bool open(u32 index, u32 lanes, TextBuf* err, double* compile_ms = nullptr);
    void close();
    bool is_open() const { return impl_ != nullptr; }
    const char* name() const;
    // One round, in two halves so the CPU can work while the GPU does. The
    // GPU keeps every node's lanes between rounds (by node and warp slot):
    // submit() sends the jobs' images and the lanes listed in each job's
    // `send`, and starts every record running; finish() waits for it and
    // copies the images back. `pool` (if given) does the copying in parallel.
    // False on a device error: the lanes the GPU held are lost with it.
    bool submit(const u32* cst, u32 ncst, const WarpJob* jobs, u32 njobs, TextBuf* err,
                class ThreadPool* pool = nullptr);
    bool finish(TextBuf* err, class ThreadPool* pool = nullptr);
    // Brings warps' lanes from the GPU into the host's copies (n of them).
    bool fetch(const WarpFetch* list, u32 n, TextBuf* err);
    // submit() then finish(): the round with nothing in between.
    bool run(const u32* cst, u32 ncst, const WarpJob* jobs, u32 njobs, TextBuf* err, class ThreadPool* pool = nullptr) {
        return submit(cst, ncst, jobs, njobs, err, pool) && finish(err, pool);
    }
    // The device's time for the last round (its timestamps), or 0.
    double last_ms() const { return last_ms_; }

  private:
    struct Impl;
    Impl* impl_ = nullptr;
    WarpGpuStats stats_;
    double last_ms_ = 0;
};

// The NPU backend: dot, matvec and matmul through DirectML as float16 matrix
// multiplies of base-8 digits (the only exact form the NPU takes, C6), put
// back together exactly on the CPU; argmax, which has no multiplies, on the
// CPU. The same interface and results as the GPU backend.
class NpuBackend {
  public:
    NpuBackend() = default;
    ~NpuBackend() { close(); }
    NpuBackend(const NpuBackend&) = delete;
    NpuBackend& operator=(const NpuBackend&) = delete;

    // Opens device `index` (accel_list order), compiles its multiplies and
    // runs a known-answer self-test of every op kind at every batch size
    // through the real path. False (with the reason) if anything is refused or
    // any answer is wrong. `compile_ms`, if given, gets the start-up time.
    bool open(u32 index, TextBuf* err, double* compile_ms = nullptr);
    void close();
    bool is_open() const { return impl_ != nullptr; }
    const char* name() const;

    // As GpuBackend: only NPU kinds (dot, matvec, matmul, argmax).
    int submit(const AccelOp* ops, const u32* pick, u32 n, const u8* in);
    bool collect(int ticket, u8* codons, u32* scalars);
    // Why the last submit or collect failed.
    const char* why() const;

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace nova
