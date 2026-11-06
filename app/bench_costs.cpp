// nova bench costs (Phase 9, design.md 6.15, docs/phase9-plan.md C1, C8, C9):
// times this machine's own basic operations, outside the simulation, for the
// fixed cost table home_v1 (core/src/cost.cpp). Native code, as if Nova's
// instructions were the machine's own:
//
//   alu    an add (a dependent chain of adds and xors)
//   mul    a multiply (a dependent chain)
//   div    a 32-bit signed divide (a dependent chain, less the add between)
//   load   a load from the nearest cache (a pointer chase in 4 KB)
//   store  a store (volatile stores, so none is merged)
//   jump   what a jump the hardware can't predict costs over one it can
//   scan   a codon searched for a two-codon pattern (the compiler's best loop)
//   move   a codon moved in bulk (memcpy of 5 bytes a codon: the codon and
//          its owner, as Nova keeps them)
//   draw   a random draw (splitmix64, fed back)
//   call   a kernel call (VirtualQuery: a round trip into the kernel)
//   share  a cache line passed between two P-cores (a ping-pong)
//   disk   a random 4 KB read and write on the drive holding the current
//          folder, and a codon (byte) read and written in sequence, all
//          unbuffered (no Windows file cache)
//
// Phase 9b, the devices:
//   gpu    a lane's dependent add, multiply, divide and loads (4 KB, 1 MB,
//          256 MB), timed on the GPU's own clock (gpu_microbench), and the
//          lane-ops a second of the whole GPU busy; per lane, for the GPU's
//          shader cores (10,752 on an RTX 5080: D3D12 can't say, so the
//          count is an argument).
//   start  a lone op's round trip, on the GPU and the NPU: what spinning a
//          device up for one op costs
//   share  what each further op adds when many go together (64 small ops in
//          one submission on the GPU, 16 on the NPU, less one, over the rest)
//   mac    a multiply-add on the NPU, from a mix of matrix ops
//
// CPU work runs pinned to a P-core, then to an E-core. Each figure is the
// median of 9 repeats; a spread over 10% is flagged (a busy machine). The
// slower levels of memory are printed for the record. Runs never measure:
// these numbers are copied into the source as a named table.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nova/accel.h"
#include "nova/accelop.h"

namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;

double now_s() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// Inputs the compiler can't see through, and a sink it must write.
volatile u32 g_seed = 0x9E3779B9u, g_three = 3u, g_big = 0x40000000u, g_a = 61u, g_b = 62u;
volatile u64 g_sink = 0;
volatile u32 g_hit = 0;

// --- the kernels: each runs n iterations and returns seconds --------------

double k_alu(u64 n) { // 8 dependent one-cycle ops an iteration
    u32 x = g_seed, y = g_three, z = g_seed >> 3;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        x += y;
        x ^= z;
        x += y;
        x ^= z;
        x += y;
        x ^= z;
        x += y;
        x ^= z;
    }
    const double s = now_s() - t;
    g_sink = x;
    return s;
}

double k_mul(u64 n) { // 8 dependent (multiply, xor) pairs: an xor between, so the
                      // compiler can't fold the chain into one multiply by k^8
    u32 x = g_seed, k = g_three | 1u, y = g_seed >> 5;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
        x = (x * k) ^ y;
    }
    const double s = now_s() - t;
    g_sink = x;
    return s;
}

double k_div(u64 n) { // 4 dependent (divide, add) pairs
    i32 x = static_cast<i32>(g_big);
    const i32 d = static_cast<i32>(g_three), b = static_cast<i32>(g_big);
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        x = x / d + b;
        x = x / d + b;
        x = x / d + b;
        x = x / d + b;
    }
    const double s = now_s() - t;
    g_sink = static_cast<u64>(static_cast<u32>(x));
    return s;
}

// A random single cycle through `words` slots (Sattolo's shuffle).
u32* make_chase(u64 words) {
    u32* a = static_cast<u32*>(VirtualAlloc(nullptr, words * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!a)
        return nullptr;
    for (u64 i = 0; i < words; ++i)
        a[i] = static_cast<u32>(i);
    u64 r = 0x2545F4914F6CDD1Dull;
    for (u64 i = words - 1; i > 0; --i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        const u64 j = r % i;
        const u32 t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    return a;
}

const u32* g_chase = nullptr;
double k_load(u64 n) { // 8 dependent loads
    const u32* a = g_chase;
    u32 p = g_seed & 0;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        p = a[p];
        p = a[p];
        p = a[p];
        p = a[p];
        p = a[p];
        p = a[p];
        p = a[p];
        p = a[p];
    }
    const double s = now_s() - t;
    g_sink = p;
    return s;
}

u32 g_store_buf[1024];
double k_store(u64 n) { // 8 volatile stores
    volatile u32* v = g_store_buf;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        const u32 b = static_cast<u32>(i * 8) & 1016u;
        v[b] = static_cast<u32>(i);
        v[b + 1] = static_cast<u32>(i);
        v[b + 2] = static_cast<u32>(i);
        v[b + 3] = static_cast<u32>(i);
        v[b + 4] = static_cast<u32>(i);
        v[b + 5] = static_cast<u32>(i);
        v[b + 6] = static_cast<u32>(i);
        v[b + 7] = static_cast<u32>(i);
    }
    return now_s() - t;
}

u8 g_bits_rand[1u << 16], g_bits_alt[1u << 16];
double k_branch(const u8* bits, u64 n) { // one branch an iteration, a volatile store on one side
    u32 x = g_seed, y = 0;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        if (bits[i & 0xFFFFu]) {
            x = x * 3u + 1u;
            g_hit = x;
        } else {
            x = (x >> 1) ^ 0x5BD1E995u;
        }
        y += x;
    }
    const double s = now_s() - t;
    g_sink = y;
    return s;
}
double k_branch_rand(u64 n) { return k_branch(g_bits_rand, n); }
double k_branch_alt(u64 n) { return k_branch(g_bits_alt, n); }

u8 g_scan_buf[16384 + 1];
double k_scan(u64 n) { // n passes over 16,384 codons
    const u8 a = static_cast<u8>(g_a), b = static_cast<u8>(g_b);
    u64 found = 0;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        for (u32 j = 0; j < 16384; ++j)
            found += g_scan_buf[j] == a && g_scan_buf[j + 1] == b;
        g_sink = found;
    }
    return now_s() - t;
}

u8 g_move_src[4096 * 5], g_move_dst[4096 * 5];
double k_move(u64 n) { // n copies of 4,096 codons (5 bytes each)
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        memcpy(g_move_dst, g_move_src, sizeof g_move_src);
        g_move_src[i & 4095] ^= g_move_dst[(i * 7) & 4095];
    }
    const double s = now_s() - t;
    g_sink = g_move_dst[17];
    return s;
}

double k_draw(u64 n) { // splitmix64, each draw feeding the next
    u64 x = g_seed;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        u64 z = (x += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        x ^= z;
    }
    const double s = now_s() - t;
    g_sink = x;
    return s;
}

double k_call(u64 n) { // a kernel round trip each
    MEMORY_BASIC_INFORMATION m;
    u64 acc = 0;
    const double t = now_s();
    for (u64 i = 0; i < n; ++i) {
        VirtualQuery(const_cast<const u64*>(&g_sink), &m, sizeof m);
        acc += m.RegionSize;
    }
    const double s = now_s() - t;
    g_sink = acc;
    return s;
}

// --- running a kernel well -----------------------------------------------

struct Kernel {
    const char* name;
    double (*fn)(u64);
    double per; // operations an iteration
};

int cmp_double(const void* a, const void* b) {
    const double x = *static_cast<const double*>(a), y = *static_cast<const double*>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Picoseconds an operation: the median of 9 repeats of about 30 ms each.
// *spread is (slowest - fastest) / median.
double measure(const Kernel& k, double* spread) {
    u64 n = 1000;
    for (;;) { // grow n until one repeat takes 30 ms
        const double s = k.fn(n);
        if (s >= 0.03 || n > (1ull << 40))
            break;
        n = s < 0.003 ? n * 10 : static_cast<u64>(static_cast<double>(n) * 0.03 / s) + 1;
    }
    double v[9];
    for (int r = 0; r < 9; ++r)
        v[r] = k.fn(n) * 1e12 / (static_cast<double>(n) * k.per);
    qsort(v, 9, sizeof v[0], cmp_double);
    *spread = v[4] > 0 ? (v[8] - v[0]) / v[4] : 0;
    return v[4];
}

// One logical processor of the most and of the least efficient cores (the
// P-cores and the E-cores; equal on a machine with one kind).
bool core_masks(KAFFINITY* p_mask, KAFFINITY* e_mask, int* p_count, int* e_count) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
        return false;
    u8* buf = static_cast<u8*>(malloc(len));
    if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                  reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf),
                                                  &len)) {
        free(buf);
        return false;
    }
    int hi = -1, lo = 1000;
    for (DWORD off = 0; off < len;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf + off);
        const int c = e->Processor.EfficiencyClass;
        hi = c > hi ? c : hi;
        lo = c < lo ? c : lo;
        off += e->Size;
    }
    *p_mask = *e_mask = 0;
    *p_count = *e_count = 0;
    for (DWORD off = 0; off < len;) {
        const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf + off);
        const KAFFINITY m = e->Processor.GroupMask[0].Mask;
        const KAFFINITY first = m & (~m + 1); // one logical processor of the core
        if (e->Processor.EfficiencyClass == hi) {
            ++*p_count;
            if (!*p_mask)
                *p_mask = first;
        }
        if (e->Processor.EfficiencyClass == lo) {
            ++*e_count;
            if (!*e_mask)
                *e_mask = first;
        }
        off += e->Size;
    }
    free(buf);
    return *p_mask != 0;
}

// A second P-core's processor, for the cache-line ping-pong.
KAFFINITY second_p_core() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    u8* buf = static_cast<u8*>(malloc(len ? len : 1));
    KAFFINITY got = 0, first_seen = 0;
    int hi = -1;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf), &len)) {
        for (DWORD off = 0; off < len;) {
            const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf + off);
            hi = e->Processor.EfficiencyClass > hi ? e->Processor.EfficiencyClass : hi;
            off += e->Size;
        }
        for (DWORD off = 0; off < len && !got;) {
            const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf + off);
            if (e->Processor.EfficiencyClass == hi) {
                const KAFFINITY m = e->Processor.GroupMask[0].Mask;
                const KAFFINITY first = m & (~m + 1);
                if (!first_seen)
                    first_seen = first;
                else
                    got = first;
            }
            off += e->Size;
        }
    }
    free(buf);
    return got;
}

// --- a cache line between two cores ----------------------------------------

struct alignas(64) Line {
    volatile LONG v;
    char pad[60];
};
Line g_line;
LONG g_rounds = 0;

DWORD WINAPI pong(void*) {
    const LONG n = g_rounds;
    for (LONG i = 1; i < 2 * n; i += 2) {
        while (g_line.v != i)
            _mm_pause();
        g_line.v = i + 1;
    }
    return 0;
}

// Picoseconds a hand-over (the line moving one way), median of 5.
double measure_share(KAFFINITY a, KAFFINITY b, double* spread) {
    double v[5];
    const LONG n = 200000;
    for (int r = 0; r < 5; ++r) {
        g_line.v = 0;
        g_rounds = n;
        HANDLE h = CreateThread(nullptr, 0, pong, nullptr, CREATE_SUSPENDED, nullptr);
        if (!h)
            return 0;
        SetThreadAffinityMask(h, b);
        SetThreadAffinityMask(GetCurrentThread(), a);
        ResumeThread(h);
        const double t = now_s();
        for (LONG i = 0; i < 2 * n; i += 2) {
            while (g_line.v != i)
                _mm_pause();
            g_line.v = i + 1;
        }
        while (g_line.v != 2 * n)
            _mm_pause();
        v[r] = (now_s() - t) * 1e12 / (2.0 * n);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    qsort(v, 5, sizeof v[0], cmp_double);
    *spread = v[2] > 0 ? (v[4] - v[0]) / v[2] : 0;
    return v[2];
}

// --- the disk ----------------------------------------------------------------

struct DiskResult {
    double read_ps = 0, write_ps = 0;             // a random 4 KB access
    double read_codon_ps = 0, write_codon_ps = 0; // a byte in sequence
    bool ok = false;
};

double median_of(double* v, int n) {
    qsort(v, static_cast<size_t>(n), sizeof v[0], cmp_double);
    return v[n / 2];
}

DiskResult measure_disk() {
    DiskResult d;
    const u64 file_bytes = 256ull << 20, chunk = 1ull << 20;
    HANDLE f = CreateFileA("nova_bench_costs.tmp", GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return d;
    u8* buf = static_cast<u8*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) {
        CloseHandle(f);
        return d;
    }
    for (u64 i = 0; i < chunk; ++i)
        buf[i] = static_cast<u8>(i * 131u + 7u);
    DWORD done = 0;
    bool ok = true;
    // Sequential write, then sequential read.
    double t = now_s();
    for (u64 at = 0; at < file_bytes && ok; at += chunk)
        ok = WriteFile(f, buf, static_cast<DWORD>(chunk), &done, nullptr) && done == chunk;
    d.write_codon_ps = (now_s() - t) * 1e12 / static_cast<double>(file_bytes);
    LARGE_INTEGER zero = {};
    ok = ok && SetFilePointerEx(f, zero, nullptr, FILE_BEGIN);
    t = now_s();
    for (u64 at = 0; at < file_bytes && ok; at += chunk)
        ok = ReadFile(f, buf, static_cast<DWORD>(chunk), &done, nullptr) && done == chunk;
    d.read_codon_ps = (now_s() - t) * 1e12 / static_cast<double>(file_bytes);
    // Random 4 KB reads and writes.
    u64 r = 0x9E3779B97F4A7C15ull;
    const u64 pages = file_bytes / 4096;
    static double rv[2001], wv[501];
    for (int i = 0; i < 2001 && ok; ++i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>((r % pages) * 4096);
        t = now_s();
        ok = SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, buf, 4096, &done, nullptr) && done == 4096;
        rv[i] = (now_s() - t) * 1e12;
    }
    for (int i = 0; i < 501 && ok; ++i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>((r % pages) * 4096);
        t = now_s();
        ok = SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && WriteFile(f, buf, 4096, &done, nullptr) && done == 4096;
        wv[i] = (now_s() - t) * 1e12;
    }
    if (ok) {
        d.read_ps = median_of(rv, 2001);
        d.write_ps = median_of(wv, 501);
        d.ok = true;
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(f); // deletes the file
    return d;
}

const char* flag(double spread) { return spread > 0.10 ? "  (busy?)" : ""; }

// --- the devices' start-up and share (Phase 9b) ----------------------------

// `count` ops of one kind, each of size n, with random codons.
struct OpBatch {
    nova::Array<nova::AccelOp> ops;
    nova::Array<u8> in;
    nova::Array<u32> pick;
    u32 outs = 0;
    u64 macs = 0;
};

void add_batch(OpBatch* b, u32 kind, u32 count, u32 n, u32* seed) {
    for (u32 i = 0; i < count; ++i) {
        nova::AccelOp op;
        memset(&op, 0, sizeof op);
        op.kind = kind;
        op.n = n;
        const bool two = kind == nova::kAccCompare || (kind >= nova::kAccAdd && kind <= nova::kAccDot);
        op.src_n = kind == nova::kAccFill ? 0 : (kind == nova::kAccMatmul ? n * n : n);
        op.dst_n = two ? n : 0;
        op.mat_n = kind == nova::kAccMatvec || kind == nova::kAccMatmul ? n * n : 0;
        op.out_n = nova::accel_out_codons(kind, n);
        op.key = 7;
        op.in_off = static_cast<u32>(b->in.size());
        for (u32 j = 0; j < op.src_n + op.dst_n + op.mat_n; ++j) {
            *seed = *seed * 1664525u + 1013904223u;
            b->in.push(static_cast<u8>((*seed >> 16) % 64));
        }
        b->pick.push(static_cast<u32>(b->ops.size()));
        b->ops.push(op);
        b->outs += op.out_n;
        b->macs += kind == nova::kAccMatmul ? static_cast<u64>(n) * n * n
                                            : (kind == nova::kAccMatvec ? static_cast<u64>(n) * n : n);
    }
}

// Picoseconds a submission and its collection take, the median of `reps`.
template <class Dev> double time_batch(Dev& d, const OpBatch& b, int reps) {
    nova::Array<u8> out;
    nova::Array<u32> scal;
    out.resize(b.outs ? b.outs : 1);
    scal.resize(b.ops.size());
    double v[64];
    reps = reps > 64 ? 64 : reps;
    for (int i = -2; i < reps; ++i) { // two to warm up
        const double t = now_s();
        const int tk = d.submit(b.ops.data(), b.pick.data(), static_cast<u32>(b.pick.size()), b.in.data());
        if (tk < 0 || !d.collect(tk, out.data(), scal.data()))
            return 0;
        if (i >= 0)
            v[i] = (now_s() - t) * 1e12;
    }
    qsort(v, static_cast<size_t>(reps), sizeof v[0], cmp_double);
    return v[reps / 2];
}

struct DeviceCosts {
    nova::GpuMicro micro = {};
    double gpu_thr = 0;                       // ps a lane-op, per lane, the whole GPU busy
    double gpu_start = 0, gpu_share = 0;      // a lone op's round trip; each further op in a batch
    double npu_start = 0, npu_share = 0, npu_mac = 0;
    bool gpu = false, npu = false;
    char gpu_name[128] = "", npu_name[128] = "";
};

DeviceCosts measure_devices(u32 lanes) {
    DeviceCosts dc;
    nova::AccelDevice devs[16];
    const u32 nd = nova::accel_list(devs, 16);
    u32 gi = 0xFFFFFFFFu, ni = 0xFFFFFFFFu;
    for (u32 i = 0; i < nd; ++i) {
        if (devs[i].gpu && devs[i].hardware && gi == 0xFFFFFFFFu)
            gi = i;
        if (devs[i].npu && devs[i].hardware && ni == 0xFFFFFFFFu)
            ni = i;
    }
    nova::TextBuf err;
    u32 seed = 77;
    if (gi != 0xFFFFFFFFu) {
        memcpy(dc.gpu_name, devs[gi].name, sizeof dc.gpu_name);
        if (nova::gpu_microbench(gi, &dc.micro, &err)) {
            dc.gpu_thr = static_cast<double>(lanes) / dc.micro.ops_per_s * 1e12;
            nova::GpuBackend gpu;
            err.clear();
            if (gpu.open(gi, &err)) {
                OpBatch one, many;
                add_batch(&one, nova::kAccCopy, 1, 32, &seed);
                add_batch(&many, nova::kAccCopy, 64, 32, &seed);
                dc.gpu_start = time_batch(gpu, one, 31);
                const double t64 = time_batch(gpu, many, 31);
                dc.gpu_share = t64 > dc.gpu_start ? (t64 - dc.gpu_start) / 63.0 : 0.0;
                dc.gpu = dc.gpu_start > 0;
            }
        }
        if (!dc.gpu)
            fprintf(stderr, "nova bench costs: GPU %s: %s\n", devs[gi].name, err.c_str());
    }
    if (ni != 0xFFFFFFFFu) {
        memcpy(dc.npu_name, devs[ni].name, sizeof dc.npu_name);
        nova::NpuBackend npu;
        err.clear();
        double startup = 0;
        if (npu.open(ni, &err, &startup)) {
            OpBatch one, many, mix;
            add_batch(&one, nova::kAccMatvec, 1, 16, &seed);
            add_batch(&many, nova::kAccMatvec, 16, 16, &seed);
            add_batch(&mix, nova::kAccMatvec, 4, 256, &seed); // the bench devices mix: 8.7 M MACs
            add_batch(&mix, nova::kAccMatmul, 32, 64, &seed);
            add_batch(&mix, nova::kAccDot, 16, 4096, &seed);
            dc.npu_start = time_batch(npu, one, 9);
            const double t16 = time_batch(npu, many, 9);
            dc.npu_share = t16 > dc.npu_start ? (t16 - dc.npu_start) / 15.0 : 0.0;
            const double tmix = time_batch(npu, mix, 5);
            dc.npu_mac = mix.macs ? tmix / static_cast<double>(mix.macs) : 0.0;
            dc.npu = dc.npu_start > 0;
        }
        if (!dc.npu)
            fprintf(stderr, "nova bench costs: NPU %s: %s\n", devs[ni].name, err.c_str());
    }
    return dc;
}

} // namespace

int cmd_bench_costs() {
    KAFFINITY pm = 0, em = 0;
    int pc = 0, ec = 0;
    if (!core_masks(&pm, &em, &pc, &ec)) {
        fprintf(stderr, "nova bench costs: could not read the processor layout\n");
        return 1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Inputs.
    u64 r = 0x853C49E6748FEA9Bull;
    for (u32 i = 0; i < (1u << 16); ++i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        g_bits_rand[i] = static_cast<u8>(r & 1);
        g_bits_alt[i] = static_cast<u8>(i & 1);
    }
    for (u32 i = 0; i < sizeof g_scan_buf; ++i) {
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        g_scan_buf[i] = static_cast<u8>(r % 61); // never 61 or 62: the pattern is never found
    }
    for (u32 i = 0; i < sizeof g_move_src; ++i)
        g_move_src[i] = static_cast<u8>(i);

    const Kernel ks[] = {
        {"alu", k_alu, 8},     {"mul", k_mul, 8},       {"div", k_div, 4},      {"load", k_load, 8},
        {"store", k_store, 8}, {"jump", nullptr, 1},    {"scan", k_scan, 16384}, {"move", k_move, 4096},
        {"draw", k_draw, 1},   {"call", k_call, 1},
    };
    const int nk = static_cast<int>(sizeof ks / sizeof ks[0]);
    double val[2][16] = {}, spr[2][16] = {};
    g_chase = make_chase(1024); // 4 KB: the nearest cache
    for (int side = 0; side < 2; ++side) {
        SetThreadAffinityMask(GetCurrentThread(), side == 0 ? pm : em);
        Sleep(20);
        for (int i = 0; i < nk; ++i) {
            if (ks[i].fn) {
                val[side][i] = measure(ks[i], &spr[side][i]);
            } else { // a jump: random against alternating, half of the random ones mispredicted
                double s1 = 0, s2 = 0;
                const double rnd = measure(Kernel{"", k_branch_rand, 1}, &s1);
                const double alt = measure(Kernel{"", k_branch_alt, 1}, &s2);
                val[side][i] = (rnd - alt) * 2.0;
                spr[side][i] = s1 > s2 ? s1 : s2;
            }
        }
    }
    // mul and div: less the xor or add between them.
    for (int side = 0; side < 2; ++side) {
        val[side][1] -= val[side][0];
        val[side][2] -= val[side][0];
    }

    // The slower memory levels, for the record (P-core).
    SetThreadAffinityMask(GetCurrentThread(), pm);
    const u64 levels[] = {16384, 262144, 4194304, 67108864}; // 64 KB, 1 MB, 16 MB, 256 MB
    double lev[4] = {}, lev_s[4] = {};
    for (int i = 0; i < 4; ++i) {
        u32* a = make_chase(levels[i]);
        if (!a)
            continue;
        g_chase = a;
        lev[i] = measure(Kernel{"", k_load, 8}, &lev_s[i]);
        VirtualFree(a, 0, MEM_RELEASE);
    }

    double share_s = 0;
    const KAFFINITY p2 = second_p_core();
    const double share = p2 ? measure_share(pm, p2, &share_s) : 0;
    const DiskResult disk = measure_disk();
    DWORD_PTR proc_mask = 0, sys_mask = 0; // the devices: free to run anywhere again
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask))
        SetThreadAffinityMask(GetCurrentThread(), proc_mask);
    const u32 lanes = 10752; // the RTX 5080's shader cores
    const DeviceCosts dc = measure_devices(lanes);

    printf("nova bench costs: this machine's basic operations, picoseconds each (median of 9)\n");
    printf("  %d P-cores and %d E-cores; CPU work pinned to one of each in turn\n\n", pc, ec);
    printf("  %-8s %10s %10s\n", "", "P-core", "E-core");
    for (int i = 0; i < nk; ++i)
        printf("  %-8s %10.0f %10.0f%s%s\n", ks[i].name, val[0][i], val[1][i], flag(spr[0][i]), flag(spr[1][i]));
    printf("\n  the P-core's clock, from alu: about %.2f GHz; the E-core's: about %.2f GHz\n",
           val[0][0] > 0 ? 1000.0 / val[0][0] : 0.0, val[1][0] > 0 ? 1000.0 / val[1][0] : 0.0);
    printf("  a load further out (P-core): 64 KB %.0f, 1 MB %.0f, 16 MB %.0f, 256 MB %.0f\n", lev[0], lev[1], lev[2],
           lev[3]);
    printf("  share (a cache line, P-core to P-core): %.0f%s\n", share, flag(share_s));
    if (disk.ok)
        printf("  disk (this folder's drive, unbuffered): a random 4 KB read %.1f us, write %.1f us; in sequence "
               "%.0f ps a byte read, %.0f written\n",
               disk.read_ps / 1e6, disk.write_ps / 1e6, disk.read_codon_ps, disk.write_codon_ps);
    else
        printf("  disk: could not measure (needs room for a 256 MB file in this folder)\n");
    if (dc.gpu) {
        printf("\n  GPU %s, a lane's dependent op on its own clock:\n", dc.gpu_name);
        printf("    add %.0f, multiply %.0f, divide %.0f; a load from 4 KB %.0f, 1 MB %.0f, 256 MB %.0f; "
               "a place searched %.0f\n",
               dc.micro.alu, dc.micro.mul, dc.micro.div, dc.micro.l1, dc.micro.mem, dc.micro.vram, dc.micro.scan);
        printf("    the whole GPU busy: %.3g lane-ops a second, so %.0f ps a lane-op per lane (%u lanes)\n",
               dc.micro.ops_per_s, dc.gpu_thr, lanes);
        printf("    a lone op's round trip %.1f us; each further op in a batch of 64 adds %.2f us\n",
               dc.gpu_start / 1e6, dc.gpu_share / 1e6);
    }
    if (dc.npu)
        printf("  NPU %s: a lone op's round trip %.2f ms; each further op in a batch of 16 adds %.3f ms; "
               "%.0f ps a multiply-add\n",
               dc.npu_name, dc.npu_start / 1e9, dc.npu_share / 1e9, dc.npu_mac);
    printf("\n  as a table (core/src/cost.cpp, P-core), picoseconds:\n");
    printf("    alu %.0f, mul %.0f, div %.0f, load %.0f, store %.0f, jump %.0f, scan %.0f, move %.0f, draw %.0f,\n"
           "    call %.0f, share %.0f, disk_read %.0f, disk_write %.0f, disk_read_codon %.0f, disk_write_codon %.0f\n",
           val[0][0], val[0][1], val[0][2], val[0][3], val[0][4], val[0][5], val[0][6], val[0][7], val[0][8],
           val[0][9], share, disk.read_ps, disk.write_ps, disk.read_codon_ps, disk.write_codon_ps);
    printf("    gpu_alu %.0f, gpu_mul %.0f, gpu_div %.0f, gpu_l1 %.0f, gpu_mem %.0f, gpu_vram %.0f, gpu_thr %.0f,\n"
           "    gpu_start %.0f, gpu_share %.0f, npu_start %.0f, npu_share %.0f, npu_mac %.0f, gpu_scan %.0f\n",
           dc.micro.alu, dc.micro.mul, dc.micro.div, dc.micro.l1, dc.micro.mem, dc.micro.vram, dc.gpu_thr, dc.gpu_start,
           dc.gpu_share, dc.npu_start, dc.npu_share, dc.npu_mac, dc.micro.scan);
    return 0;
}
