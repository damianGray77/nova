// Phase 1b pilot driver (THROWAWAY -- see README.md). Not part of the engine
// build; build.ps1 compiles it against a patched copy of core/.
//
// Runs one soup node and prints its health at a few tick marks:
//   free memory, average free CPU pool over the interval, bodies, sparks,
//   births, and -- when a seed genome is given -- the size of its lineage
//   (exact copies, copies matching >= 75% of positions, millicores they hold,
//   the richest copy).
//
// usage: diag "<config overrides, ';'-separated> | -" [seed] [max tick, default 50000]
//   e.g. diag "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 SENSE DIVIDE;seed.at = 30000;seed.millicores = 10" 7
// Any key given in the overrides replaces the default below. The pilot rules
// themselves are switched by environment variables (README.md).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nova/run.h"

using namespace nova;

namespace {

// The bench node of design.md 7.1 I, sparks-only (Phase 1a defaults).
const char* const kDefaults[] = {
    "world.model = soup",   "world.nodes = 1",           "world.chords = 0",
    "world.soup_size = 65536", "world.millicores = 4000", "cpu.instr_per_core = 25000",
    "spark.initial = 400",  "spark.rate = 4",            "spark.grant = 10",
    "spark.min_len = 8",    "spark.max_len = 64",
};

// True when the ';'-separated override list sets `key` (the text before '=').
bool overrides_key(const char* extra, const char* line) {
    usize klen = 0;
    while (line[klen] && line[klen] != ' ' && line[klen] != '=')
        ++klen;
    const char* p = extra;
    while (*p) {
        while (*p == ' ' || *p == ';')
            ++p;
        if (strncmp(p, line, klen) == 0 && (p[klen] == ' ' || p[klen] == '='))
            return true;
        while (*p && *p != ';')
            ++p;
    }
    return false;
}

void report(const Soup& s, u64 tick, u64 sparks, u64 births, double pool_avg) {
    const Array<u8>& g = s.params().seed_genome;
    u64 exact = 0, similar = 0, sim_units = 0, sim_max_units = 0;
    for (usize i = 0; i < s.body_count(); ++i) {
        const Body& B = s.body(s.body_at(i));
        if (g.empty() || B.blocks == kNone)
            continue;
        const Block& bl = s.block(B.blocks);
        if (bl.len != g.size())
            continue;
        u32 m = 0;
        for (u32 j = 0; j < bl.len; ++j)
            m += s.mem()[bl.start + j] == g[j];
        exact += m == bl.len;
        if (4 * m >= 3 * bl.len) {
            ++similar;
            sim_units += B.units;
            if (B.units > sim_max_units)
                sim_max_units = B.units;
        }
    }
    printf("t=%-6llu free_mem=%5.1f%% avg_pool=%-7.1f bodies=%-5zu sparks=%-6llu births=%-8llu",
           (unsigned long long)tick, 100.0 * s.waste() / s.size(), pool_avg, s.body_count(),
           (unsigned long long)sparks, (unsigned long long)births);
    if (!g.empty())
        printf(" lineage: exact=%-4llu 75pct=%-4llu cpu_held=%-5llu richest=%llu", (unsigned long long)exact,
               (unsigned long long)similar, (unsigned long long)sim_units, (unsigned long long)sim_max_units);
    printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    // "-" means no overrides (PowerShell 5.1 drops an empty "" argument).
    const char* extra = argc > 1 && strcmp(argv[1], "-") != 0 ? argv[1] : "";
    Run run;
    TextBuf text;
    text.append("seed = ");
    text.append(argc > 2 ? argv[2] : "7");
    text.append_char('\n');
    for (const char* line : kDefaults) {
        if (!overrides_key(extra, line)) {
            text.append(line);
            text.append_char('\n');
        }
    }
    for (const char* p = extra; *p; ++p)
        text.append_char(*p == ';' ? '\n' : *p);
    text.append_char('\n');

    Config::ParseError pe;
    TextBuf err;
    if (!run.config().parse(text.view(), &pe) || !run.init(1, &err)) {
        fprintf(stderr, "init failed: %s %s\n", pe.msg, err.c_str());
        return 1;
    }
    const u64 max_tick = argc > 3 ? strtoull(argv[3], nullptr, 10) : 50000;
    const u64 marks[] = {100, 1000, 5000, 10000, 20000, 50000};
    u64 sparks = 0, births = 0;
    for (u64 m : marks) {
        if (m > max_tick)
            break;
        double pool_sum = 0;
        u64 n = 0;
        while (run.world().tick() < m) {
            run.step();
            sparks += run.world().last_soup_tick().sparks;
            births += run.world().last_soup_tick().births;
            pool_sum += run.world().node_soup(0).free_units();
            ++n;
        }
        report(run.world().node_soup(0), m, sparks, births, n ? pool_sum / n : 0);
        fflush(stdout);
    }
    return 0;
}
