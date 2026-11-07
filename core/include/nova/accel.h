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
    const u32* recs;     // the record offsets the GPU runs (any of the image's: the rest are left as they are)
    u32 nrec;
    const void* lanes;   // the node's lanes on the host (soup.h Lane), by slot
    u32 nlanes;          // how many (warp slots x lanes)
    const u32* send;     // warp slots whose host lanes must go to the GPU first
    u32 nsend;
    // Each record's size in words, beside recs; nullptr when recs are all the
    // image's records in order (each then runs to the next, the last to the end).
    const u32* rec_words = nullptr;
    // Each record's expected chain (its warps' steps in turn), beside recs, for
    // WarpKernelOptions::long_first; nullptr: unknown.
    const u32* rec_chain = nullptr;
};

// How the warp kernel is chosen and dispatched (docs/dxc-plan.md; the
// defaults are the world's). DXC's kernel, compiled at build time, runs when
// the build has it, the world has 32 lanes and the device takes Shader Model
// 6.0; else FXC's, compiled at run time (WarpGpu::kernel() says which, and
// why). Either gives the reference's words exactly.
struct WarpKernelOptions;
// Whether this build carries DXC's warp kernels (its build found dxc.exe).
bool warp_dxc_built();

struct WarpKernelOptions {
    // Groupshared tiers: a record runs with its image in groupshared memory on
    // the smallest tier that holds it (words, ascending, 0 ends the list),
    // else on the plain kernel (its image in video memory). Each group holds
    // its tier's whole size whatever its record's, so smaller tiers fit more
    // groups on an SM: 75-79% of the steps are in records of 256 words or less.
    // DXC's tiers must be ones the build compiled (256, 512, 1,024, 2,048).
    u32 lds[4] = {256, 512, 1024, 2048};
    // FXC's (at most 7,680 words: D3D12 gives a group 32 KB). One size, as
    // before DXC: each FXC kernel takes about a minute and a half to compile.
    u32 fxc_lds[4] = {2048, 0, 0, 0};
    // The records with the longest expected chains (over a quarter of the
    // longest, from WarpJob::rec_chain) first, in dispatches of their own,
    // longest first, so they start at once. Only the order changes.
    bool long_first = true;
    // FXC's kernel even where DXC's could run (warp.kernel = fxc).
    bool fxc = false;
};

// A job's record r's size in words.
inline u32 warp_job_rec_words(const WarpJob& j, u32 r) {
    if (j.rec_words)
        return j.rec_words[r];
    return (r + 1 < j.nrec ? j.recs[r + 1] : j.img_words) - j.recs[r];
}

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
    u64 fetches = 0;                       // ... how many round trips
    u64 lanes_gathered = 0;                // warps' lanes that came back with their round (no round trip)
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
    bool open(u32 index, u32 lanes, TextBuf* err, double* compile_ms = nullptr,
              const WarpKernelOptions* options = nullptr);
    void close();
    bool is_open() const { return impl_ != nullptr; }
    const char* name() const;
    // One round, in two halves so the CPU can work while the GPU does. The
    // GPU keeps every node's lanes between rounds (by node and warp slot):
    // submit() sends the jobs' images and the lanes listed in each job's
    // `send`, and starts every record running; finish() waits for it and
    // copies the images back. `pool` (if given) does the copying in parallel.
    // False on a device error: the lanes the GPU held are lost with it.
    // Batches (the overlap plan): up to kBatches rounds in flight at once,
    // each on its own queue, for disjoint sets of nodes; the GPU may run
    // their kernels at the same time.
    static constexpr u32 kBatches = 2;
    bool submit(const u32* cst, u32 ncst, const WarpJob* jobs, u32 njobs, TextBuf* err,
                class ThreadPool* pool = nullptr, u32 batch = 0);
    bool finish(TextBuf* err, class ThreadPool* pool = nullptr, u32 batch = 0);
    // Brings warps' lanes from the GPU into the host's copies (n of them), on
    // `batch`'s queue (not in flight; the other batch may be).
    bool fetch(const WarpFetch* list, u32 n, TextBuf* err, u32 batch = 0);
    // The lanes of warps that stopped at a host instruction come back with
    // their round (gathered on the GPU after the run, into the round's own
    // readback), so most host instructions need no fetch. After finish():
    // copies those of the listed warps it has, sets got[i] for each, and
    // returns how many (the rest still need fetch()). Once a round.
    u32 take_gathered(const WarpFetch* list, u32 n, u8* got, class ThreadPool* pool = nullptr, u32 batch = 0);
    // submit() then finish(): the round with nothing in between.
    bool run(const u32* cst, u32 ncst, const WarpJob* jobs, u32 njobs, TextBuf* err, class ThreadPool* pool = nullptr) {
        return submit(cst, ncst, jobs, njobs, err, pool) && finish(err, pool);
    }
    // The device's time for the last round (its timestamps), or 0.
    double last_ms() const { return last_ms_; }
    // A batch's last round: its kernel time on the device's clock (0 without
    // timestamps) and its longest body's warp steps (a GPU runs a body's
    // steps one after another, so the one sets the other: the split's model),
    // and when it began and ended on the device's clock (in ticks of
    // timestamp_hz(); 0 without), to see two batches overlap.
    double last_kernel_ms(u32 batch = 0) const { return last_kernel_ms_[batch]; }
    u64 last_longest(u32 batch = 0) const { return last_longest_[batch]; }
    u64 last_begin(u32 batch) const { return last_span_[batch][0]; }
    u64 last_end(u32 batch) const { return last_span_[batch][1]; }
    double timestamp_hz() const;
    // Which warp kernel open() chose: DXC's (true) or FXC's, and a line saying
    // so (its groupshared tiers, the dispatch order, and why FXC if it is).
    bool kernel_dxc() const { return dxc_; }
    const char* kernel() const { return kernel_; }

  private:
    struct Impl;
    Impl* impl_ = nullptr;
    WarpGpuStats stats_;
    bool dxc_ = false;
    char kernel_[192] = {};
    double last_ms_ = 0, last_kernel_ms_[kBatches] = {0, 0};
    u64 last_longest_[kBatches] = {0, 0};
    u64 last_span_[kBatches][2] = {{0, 0}, {0, 0}};
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
