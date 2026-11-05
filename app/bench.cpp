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
//            abiogenesis workload (every instruction, searches, contests,
//            deaths and sparks).
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
};

const Workload kWorkloads[] = {
    {"tierra",
     "isa.table = tierra0\nisa.membrane = open\nheat.enabled = false\n"
     "hazard.copy = 0\nhazard.flaw = 0\nhazard.rot = 0\n"
     "seed.genome = nop1 nop1 nop1 nop1 zero not0 shl shl movDC adrb nop0 nop0 nop0 nop0 "
     "subAAC movBA adrf nop0 nop0 nop0 nop1 incA subCAB nop1 nop1 nop0 nop1 mal call nop0 "
     "nop0 nop1 nop1 divide jmpo nop0 nop0 nop1 nop0 ifz nop1 nop1 nop0 nop0 pushA pushB "
     "pushC nop1 nop0 nop1 nop0 movii decC ifz jmpo nop0 nop1 nop0 nop0 incA incB jmpo nop0 "
     "nop1 nop0 nop1 ifz nop1 nop0 nop1 nop1 popC popB popA ret nop1 nop1 nop1 nop0 ifz\n"
     "seed.at = 1000\nseed.millicores = 4000\n"},
    {"sketch", "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
               "seed.at = 1000\nseed.millicores = 4000\n"},
    {"random", "spark.initial = 400\nspark.rate = 4\nspark.grant = 10\n"},
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
        const u32 runs[2][2] = {{1, 1}, {threads, 2 * threads}};
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
