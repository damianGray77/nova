// Test entry point: `nova_tests` runs every suite; `nova_tests <suite>` runs one.
#include <cstdio>
#include <cstring>

#include "check.h"

void suite_containers();
void suite_str();
void suite_rng();
void suite_config();
void suite_pool();
void suite_world();
void suite_gate();
void suite_snapshot();
void suite_stats();
void suite_replay();
void suite_genebank();
void suite_isa();
void suite_interp();
void suite_tierra();
void suite_soup();
void suite_heat();
void suite_contact();
void suite_rules();
void suite_tables();
void suite_detect();
void suite_gpu();
void suite_npu();
void suite_dispersal();
void suite_leak();
void suite_edges();
void suite_disk();
void suite_salvage();
void suite_signals();
void suite_lifecycle();
void suite_async();
void suite_devices();
void suite_view();
void suite_viewer();
void suite_landscape();
void suite_budget();
void suite_warp();
void suite_costs();

namespace {

struct Suite {
    const char* name;
    void (*fn)();
};

const Suite kSuites[] = {
    {"containers", suite_containers},
    {"str", suite_str},
    {"rng", suite_rng},
    {"config", suite_config},
    {"pool", suite_pool},
    {"world", suite_world},
    {"gate", suite_gate},
    {"snapshot", suite_snapshot},
    {"stats", suite_stats},
    {"replay", suite_replay},
    {"genebank", suite_genebank},
    {"isa", suite_isa},
    {"interp", suite_interp},
    {"tierra", suite_tierra},
    {"soup", suite_soup},
    {"heat", suite_heat},
    {"contact", suite_contact},
    {"rules", suite_rules},
    {"tables", suite_tables},
    {"detect", suite_detect},
    {"gpu", suite_gpu},
    {"npu", suite_npu},
    {"dispersal", suite_dispersal},
    {"leak", suite_leak},
    {"edges", suite_edges},
    {"disk", suite_disk},
    {"salvage", suite_salvage},
    {"signals", suite_signals},
    {"lifecycle", suite_lifecycle},
    {"async", suite_async},
    {"devices", suite_devices},
    {"view", suite_view},
    {"viewer", suite_viewer},
    {"landscape", suite_landscape},
    {"budget", suite_budget},
    {"warp", suite_warp},
    {"costs", suite_costs},
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        for (const Suite& s : kSuites) {
            if (strcmp(argv[1], s.name) == 0) {
                s.fn();
                return ntest::summary(s.name);
            }
        }
        fprintf(stderr, "unknown suite '%s'\n", argv[1]);
        return 2;
    }
    for (const Suite& s : kSuites)
        s.fn();
    return ntest::summary("all");
}
