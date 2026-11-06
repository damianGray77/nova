// nova bench: interpreter throughput (design.md 7.1 I, Phase 1a gate 3).
//
// Three workloads, each run single-threaded on one node and then on all
// threads with two nodes per thread:
//
//   tierra   Ray's ancestor in the light Tierra mode, heat and hazards off:
//            the bare interpreter (copy loops, template jumps).
//   sketch   the 6.2 sketch replicator under Nova's rules, heat on, Ray's
//            baseline hazards: private-code checks, ALLOC, DIVIDE, heat.
//   random   a sparks-only random soup with Nova's rules, heat on: the Phase 1b
//            abiogenesis workload of configs/abio.cfg (every instruction,
//            searches, contests, deaths and sparks), with memory upkeep, CPU
//            wear, sparks last and the detector.
//   gpu      a hand-written GPU replicator (tests/helpers.h) with 400 GPU units:
//            the GPU op, the GPU ledger and heat (Phase 2).
//   npu      a hand-written COPY replicator that takes a dot product each
//            cycle, with 400 NPU units (Phase 2b).
//   random1a the same soup under the Phase 1a rules (mem.upkeep = 0), where
//            memory clogs within ~100 ticks: comparable with the Phase 1a
//            "random" figures in design.md 7.1 I.
//   world    configs/world.cfg's world (Phase 3): 100 procedural nodes of
//            random soup with varied properties and dispersal, 16,384 codons
//            and 2,000 millicores a node on average. Always 100 nodes, on one
//            thread and then on all of them.
//   world4   the same with every Phase 4 feature on at its suggested value.
//   worldacc the world workload with 200 GPU and 200 NPU units a node and
//            async accelerators on the real devices (Phase 5).
//   landscape configs/landscape.cfg's 1,000-node landscape (Phase 7).
//
// `nova bench devices` (Phase 5) times the GPU and NPU backends against the
// CPU reference on representative batches, their round trip for a single op,
// and the worldacc tick rate, and suggests gpu.elements and npu.macs that let
// a fully busy world's batch finish within a tick.
//
// The light Tierra mode runs without upkeep (Tierra has none).
//
// Each node runs 4000 millicores at 25,000 instructions per core per tick, so a
// fully occupied node executes 100k instructions a tick and the per-tick costs
// (heat over every unit and page, the death sweep, the barrier) are amortised
// the way a long run amortises them. Timing uses wall clock; build in Release
// (tools/bench.ps1) for numbers worth recording.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "nova/accel.h"
#include "nova/accelop.h"
#include "nova/run.h"

using namespace nova;

namespace {

double now_s() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

const char* const kCommon = "seed = 1\n"
                            "world.model = soup\n"
                            "world.chords = 0\n"
                            "world.soup_size = 65536\n"
                            "world.millicores = 4000\n"
                            "cpu.instr_per_core = 25000\n";

struct Workload {
    const char* name;
    const char* cfg;
    u32 nodes = 0; // fixed node count (and its own base config), or 0: 1 / 2 per thread
};

// The world workload's base: configs/world.cfg's world, without outputs.
const char* const kWorld = "seed = 1\n"
                           "world.model = soup\n"
                           "world.graph = procedural\n"
                           "world.soup_size = 16384\n"
                           "world.millicores = 2000\n"
                           "cpu.instr_per_core = 25000\n"
                           "vary.soup_size = 0.5\nvary.millicores = 0.5\nvary.cool = 0.5\n"
                           "vary.spark_rate = 0.5\nvary.radiation = 0.5\n"
                           "cpu.wear = 0.001\nspark.timing = last\n"
                           "spark.initial = 100\nspark.rate = 1\nspark.grant = 10\n";

// The same world with every Phase 4 feature on at its suggested value.
#define NOVA_WORLD4_EXTRA                                                                          \
    "heat.ambient = 0.05\nedge.corruption = 0.0002\nedge.failure = 0.0001\nworld.disk = 4096\n"    \
    "mem.volatility = 0.00001\nsalvage.ratio = 4\nsignal.channels = 16\nnode.endurance = 1e9\n"    \
    "node.starve_ticks = 2000\nworld.land = 0.8\nworld.drift = 0.00001\n"
const char* const kWorld4 = "seed = 1\n"
                            "world.model = soup\n"
                            "world.graph = procedural\n"
                            "world.soup_size = 16384\n"
                            "world.millicores = 2000\n"
                            "cpu.instr_per_core = 25000\n"
                            "vary.soup_size = 0.5\nvary.millicores = 0.5\nvary.cool = 0.5\n"
                            "vary.spark_rate = 0.5\nvary.radiation = 0.5\n"
                            "cpu.wear = 0.001\nspark.timing = last\n"
                            "spark.initial = 100\nspark.rate = 1\nspark.grant = 10\n" NOVA_WORLD4_EXTRA;

// The world with async accelerators on the real devices.
#define NOVA_WORLD_ACC "world.gpus = 200\nworld.npus = 200\naccel.mode = async\n"
const char* const kWorldAcc = "seed = 1\n"
                              "world.model = soup\n"
                              "world.graph = procedural\n"
                              "world.soup_size = 16384\n"
                              "world.millicores = 2000\n"
                              "cpu.instr_per_core = 25000\n"
                              "vary.soup_size = 0.5\nvary.millicores = 0.5\nvary.cool = 0.5\n"
                              "vary.spark_rate = 0.5\nvary.radiation = 0.5\n"
                              "cpu.wear = 0.001\nspark.timing = last\n"
                              "spark.initial = 100\nspark.rate = 1\nspark.grant = 10\n" NOVA_WORLD_ACC;

// A 1,000-node landscape (Phase 7): configs/landscape.cfg's world, without
// outputs.
const char* const kLandscape = "seed = 1\n"
                               "world.model = soup\n"
                               "world.graph = landscape\n"
                               "land.fraction = 0.3\nland.scale = 3\nland.border = 0.3\n"
                               "paths.mean = 3.5\n"
                               "bridge.count = 2\n"
                               "world.soup_size = 16384\n"
                               "world.millicores = 2000\n"
                               "cpu.instr_per_core = 25000\n"
                               "res.memory.spread = 0.5\nres.memory.smooth = 0.6\n"
                               "res.cpu.spread = 0.6\nres.cpu.smooth = 0.5\nres.cpu.barren = 0.02\n"
                               "vary.cool = 0.5\nvary.radiation = 0.5\n"
                               "spark.spread = 0.7\nspark.smooth = 0.6\nspark.barren = 0.1\nspark.hotspots = 3\n"
                               "cpu.wear = 0.001\nspark.timing = last\n"
                               "spark.initial = 100\nspark.rate = 1\nspark.grant = 10\n";

const Workload kWorkloads[] = {
    {"tierra",
     "isa.table = tierra0\nisa.membrane = open\nheat.enabled = false\n"
     "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\nmem.upkeep = 0\n"
     "seed.genome = nop1 nop1 nop1 nop1 zero not0 shl shl movDC adrb nop0 nop0 nop0 nop0 "
     "subAAC movBA adrf nop0 nop0 nop0 nop1 incA subCAB nop1 nop1 nop0 nop1 mal call nop0 "
     "nop0 nop1 nop1 divide jmpo nop0 nop0 nop1 nop0 ifz nop1 nop1 nop0 nop0 pushA pushB "
     "pushC nop1 nop0 nop1 nop0 movii decC ifz jmpo nop0 nop1 nop0 nop0 incA incB jmpo nop0 "
     "nop1 nop0 nop1 ifz nop1 nop0 nop1 nop1 popC popB popA ret nop1 nop1 nop1 nop0 ifz\n"
     "seed.at = 1000\nseed.millicores = 4000\n"},
    {"sketch", "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
               "seed.at = 1000\nseed.millicores = 4000\n"},
    {"random", "spark.initial = 400\nspark.rate = 4\nspark.grant = 10\ncpu.wear = 0.001\n"
               "spark.timing = last\n"},
    {"gpu", "world.gpus = 400\ngpu.wear = 0.001\nseed.genome = SENSE PUSH ALLOC ZERO INC SHL SHL ROTB "
            "POP GPU ZERO ROTB SENSE DIVIDE\nseed.at = 1000\nseed.millicores = 4000\nseed.gpus = 400\n"},
    {"npu", "world.npus = 400\nnpu.wear = 0.001\nseed.genome = SENSE PUSH ALLOC ZERO INC SHL SHL SHL "
            "SHL ROTB POP GPU ZERO ROTB SENSE NOP1 COPY DJNZ NOP0 SENSE DIVIDE\nseed.at = 1000\n"
            "seed.millicores = 4000\nseed.npus = 400\n"},
    {"random1a", "spark.initial = 400\nspark.rate = 4\nspark.grant = 10\nmem.upkeep = 0\n"},
    {"world", kWorld, 100},
    {"world4", kWorld4, 100},
    {"worldacc", kWorldAcc, 100},
    {"landscape", kLandscape, 1000},
};

struct Result {
    u64 instructions = 0;
    u64 ticks = 0;
    double seconds = 0;
    bool ok = false;
};

Result measure(const Workload& w, u32 nodes, u32 threads, double seconds) {
    Result res;
    Run run;
    TextBuf text;
    if (!w.nodes)
        text.append(kCommon);
    text.append(w.cfg);
    text.append("world.nodes = ");
    text.append_u64(nodes);
    text.append_char('\n');
    Config::ParseError pe;
    TextBuf err;
    if (!run.config().parse(text.view(), &pe) || !run.init(threads, &err)) {
        fprintf(stderr, "nova bench: %s: %s%s\n", w.name, pe.msg, err.c_str());
        return res;
    }
    for (u32 i = 0; i < 3; ++i) // warm-up: caches, first allocations, populations
        run.step();
    double t0 = now_s();
    do {
        run.step();
        res.instructions += run.world().last_soup_tick().instructions;
        ++res.ticks;
        res.seconds = now_s() - t0;
    } while (res.seconds < seconds);
    res.ok = true;
    return res;
}

} // namespace

int cmd_bench(double seconds, u32 threads) {
    if (threads == 0)
        threads = ThreadPool::hardware_threads();
#ifndef NDEBUG
    printf("nova bench: DEBUG build -- numbers are not representative (use tools/bench.ps1)\n");
#endif
    printf("nova bench: %.1f s per measurement; multi-thread runs use %u threads, %u nodes\n",
           seconds, threads, 2 * threads);
    printf("%-8s %7s %5s %14s %8s %11s %13s\n", "workload", "threads", "nodes", "instructions",
           "ticks/s", "Minstr/s", "Minstr/s/thr");
    for (const Workload& w : kWorkloads) {
        u32 runs[2][2] = {{1, 1}, {threads, 2 * threads}};
        if (w.nodes) {
            runs[0][1] = w.nodes;
            runs[1][1] = w.nodes;
        }
        for (const auto& r : runs) {
            Result res = measure(w, r[1], r[0], seconds);
            if (!res.ok)
                return 1;
            double rate = static_cast<double>(res.instructions) / res.seconds / 1e6;
            printf("%-8s %7u %5u %14llu %8.1f %11.1f %13.1f\n", w.name, r[0], r[1],
                   static_cast<unsigned long long>(res.instructions),
                   static_cast<double>(res.ticks) / res.seconds, rate, rate / r[0]);
        }
    }
    return 0;
}

namespace {

// A batch of ops with random codons: `count` ops cycling through `kinds`,
// each of size n. work = elements (GPU kinds) or multiply-adds (NPU kinds).
struct Batch {
    Array<AccelOp> ops;
    Array<u8> in;
    Array<u32> pick;
    u32 outs = 0;
    u64 work = 0;
};

void add_ops(Batch* b, const u32* kinds, u32 nkinds, u32 count, u32 n, u32* seed) {
    for (u32 i = 0; i < count; ++i) {
        AccelOp op;
        memset(&op, 0, sizeof op);
        op.kind = kinds[i % nkinds];
        op.n = n;
        const bool two = op.kind == kAccCompare || (op.kind >= kAccAdd && op.kind <= kAccDot);
        op.src_n = op.kind == kAccFill ? 0 : (op.kind == kAccMatmul ? n * n : n);
        op.dst_n = two ? n : 0;
        op.mat_n = op.kind == kAccMatvec || op.kind == kAccMatmul ? n * n : 0;
        op.out_n = accel_out_codons(op.kind, n);
        op.key = 7;
        op.in_off = static_cast<u32>(b->in.size());
        for (u32 j = 0; j < op.src_n + op.dst_n + op.mat_n; ++j) {
            *seed = *seed * 1664525u + 1013904223u;
            b->in.push(static_cast<u8>((*seed >> 16) % 64));
        }
        b->pick.push(static_cast<u32>(b->ops.size()));
        b->ops.push(op);
        b->outs += op.out_n;
        b->work += op.kind == kAccMatmul ? static_cast<u64>(n) * n * n
                                         : (op.kind == kAccMatvec ? static_cast<u64>(n) * n : n);
    }
}

// Does a device's answer equal the reference's?
bool exact(const Batch& b, const u8* got, const u32* scal) {
    Array<u8> want;
    u32 off = 0;
    for (usize i = 0; i < b.ops.size(); ++i) {
        const AccelOp& op = b.ops[i];
        want.resize(op.out_n ? op.out_n : 1);
        const u32 s = accel_reference(op, b.in.data() + op.in_off, want.data());
        if ((accel_has_scalar(op.kind) && s != scal[i]) ||
            (op.out_n && memcmp(want.data(), got + off, op.out_n) != 0))
            return false;
        off += op.out_n;
    }
    return true;
}

// Seconds per batch: the CPU reference, or a device's submit + collect.
double time_cpu(const Batch& b, double seconds) {
    Array<u8> out;
    out.resize(b.outs ? b.outs : 1);
    u32 reps = 0;
    const double t0 = now_s();
    double t = 0;
    volatile u32 sink = 0;
    do {
        u32 off = 0;
        for (usize i = 0; i < b.ops.size(); ++i) {
            sink = sink + accel_reference(b.ops[i], b.in.data() + b.ops[i].in_off, out.data() + off);
            off += b.ops[i].out_n;
        }
        ++reps;
        t = now_s() - t0;
    } while (t < seconds || reps < 3);
    return t / reps;
}

template <class Dev> double time_dev(Dev& d, const Batch& b, double seconds, bool* ok) {
    Array<u8> out;
    Array<u32> scal;
    out.resize(b.outs ? b.outs : 1);
    scal.resize(b.ops.size());
    const int t1 = d.submit(b.ops.data(), b.pick.data(), static_cast<u32>(b.pick.size()), b.in.data());
    *ok = t1 >= 0 && d.collect(t1, out.data(), scal.data()) && exact(b, out.data(), scal.data());
    if (!*ok)
        return 0;
    u32 reps = 0;
    const double t0 = now_s();
    double t = 0;
    do {
        const int tk = d.submit(b.ops.data(), b.pick.data(), static_cast<u32>(b.pick.size()), b.in.data());
        if (tk < 0 || !d.collect(tk, out.data(), scal.data())) {
            *ok = false;
            return 0;
        }
        ++reps;
        t = now_s() - t0;
    } while (t < seconds || reps < 3);
    return t / reps;
}

void row(const char* who, const char* what, double sec, u64 work, const char* unit, bool ok) {
    if (!ok) {
        printf("  %-26s %-30s failed\n", who, what);
        return;
    }
    if (work)
        printf("  %-26s %-30s %9.3f ms %10.1f M %s/s\n", who, what, sec * 1000.0, static_cast<double>(work) / sec / 1e6,
               unit);
    else
        printf("  %-26s %-30s %9.3f ms\n", who, what, sec * 1000.0);
}

} // namespace

int cmd_bench_devices(u32 threads) {
    if (threads == 0)
        threads = ThreadPool::hardware_threads();
#ifndef NDEBUG
    printf("nova bench devices: DEBUG build -- numbers are not representative (use a Release build)\n");
#endif
    const double secs = 1.0;
    AccelDevice devs[16];
    const u32 nd = accel_list(devs, 16);
    u32 gi = 0xFFFFFFFFu, ni = 0xFFFFFFFFu;
    for (u32 i = 0; i < nd; ++i) {
        if (devs[i].gpu && devs[i].hardware && gi == 0xFFFFFFFFu)
            gi = i;
        if (devs[i].npu && devs[i].hardware && ni == 0xFFFFFFFFu)
            ni = i;
    }
    GpuBackend gpu;
    NpuBackend npu;
    TextBuf err;
    double npu_start = 0;
    if (gi != 0xFFFFFFFFu && !gpu.open(gi, &err))
        printf("nova bench devices: GPU %s failed to start: %s\n", devs[gi].name, err.c_str());
    err.clear();
    if (ni != 0xFFFFFFFFu && !npu.open(ni, &err, &npu_start))
        printf("nova bench devices: NPU %s failed its self-test: %s\n", devs[ni].name, err.c_str());
    // GPU work: 64 ops of 4096 elements (the high-volume op), 8 kinds.
    // NPU work: 4 matvecs (k 256), 32 matmuls (k 64) and 16 dots of 4096: 224
    // items, near the 256 a submission takes. One op of each for latency.
    u32 seed = 99;
    const u32 gk[] = {kAccCopy, kAccAdd, kAccXor, kAccSum, kAccCount, kAccCompare, kAccMul, kAccNot};
    Batch gb, nb, g1, n1;
    add_ops(&gb, gk, 8, 64, 4096, &seed);
    const u32 mv[] = {kAccMatvec}, mm[] = {kAccMatmul}, dt[] = {kAccDot}, cp[] = {kAccCopy};
    add_ops(&nb, mv, 1, 4, 256, &seed);
    add_ops(&nb, mm, 1, 32, 64, &seed);
    add_ops(&nb, dt, 1, 16, 4096, &seed);
    add_ops(&g1, cp, 1, 1, 32, &seed);
    add_ops(&n1, mv, 1, 1, 16, &seed);
    printf("nova bench devices: %.1f s per measurement\n", secs);
    printf("  %-26s %-30s %12s %16s\n", "where", "batch", "per batch", "throughput");
    const double cpu_g = time_cpu(gb, secs), cpu_n = time_cpu(nb, secs);
    row("CPU reference (1 thread)", "64 GPU ops x 4096", cpu_g, gb.work, "elements", true);
    row("CPU reference (1 thread)", "NPU mix (8.7 M MACs)", cpu_n, nb.work, "MACs", true);
    double gpu_g = 0, npu_n = 0;
    bool ok = false;
    if (gpu.is_open()) {
        gpu_g = time_dev(gpu, gb, secs, &ok);
        row(gpu.name(), "64 GPU ops x 4096", gpu_g, gb.work, "elements", ok);
        if (!ok)
            gpu_g = 0;
        const double g_nb = time_dev(gpu, nb, secs, &ok);
        row(gpu.name(), "NPU mix (as fallback)", g_nb, nb.work, "MACs", ok);
        row(gpu.name(), "1 op: round trip", time_dev(gpu, g1, secs, &ok), 0, "", ok);
    }
    if (npu.is_open()) {
        printf("  %-26s %-30s %9.0f ms\n", npu.name(), "start-up and self-test", npu_start);
        npu_n = time_dev(npu, nb, secs, &ok);
        row(npu.name(), "NPU mix (8.7 M MACs)", npu_n, nb.work, "MACs", ok);
        if (!ok)
            npu_n = 0;
        row(npu.name(), "1 op: round trip", time_dev(npu, n1, secs, &ok), 0, "", ok);
    }
    // The world it serves: worldacc's tick rate on all threads, and the
    // capacities at which a fully busy world's batch takes one tick.
    const Workload wa = {"worldacc", kWorldAcc, 100};
    const Result res = measure(wa, 100, threads, 2 * secs);
    if (!res.ok)
        return 1;
    const double tps = static_cast<double>(res.ticks) / res.seconds;
    const double units = 200.0 * 100.0; // per node x nodes
    printf("  worldacc: %.1f ticks/s on %u threads; %.0f GPU and %.0f NPU units\n", tps, threads, units, units);
    const double g_rate = gpu_g > 0 ? static_cast<double>(gb.work) / gpu_g : static_cast<double>(gb.work) / cpu_g;
    const double n_rate = npu_n > 0 ? static_cast<double>(nb.work) / npu_n : static_cast<double>(nb.work) / cpu_n;
    printf("  suggested for a fully busy world to fit a tick: gpu.elements = %.0f, npu.macs = %.0f "
           "(defaults 4096 and 65536)\n",
           g_rate / tps / units, n_rate / tps / units);
    return 0;
}
