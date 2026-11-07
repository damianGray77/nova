// Test entry point: `nova_tests` runs every suite; `nova_tests <suite>` runs one.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "nova/config.h"

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
void suite_ecology();
void suite_keys();
void suite_substrates(); // a varied world (docs/world-plan.md W1, W2, W5)
void suite_tiers();      // disk tiers (docs/world-plan.md W4)
void suite_disasters();  // docs/disasters-plan.md
void suite_memtiers();   // memory tiers (docs/world-plan.md W3)
void suite_analysis(); // learning from the runs (docs/analysis-plan.md)
void suite_profiles(); // behaviour profiles (docs/analysis-plan.md A3)

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
    {"ecology", suite_ecology},
    {"keys", suite_keys},
    {"substrates", suite_substrates},
    {"tiers", suite_tiers},
    {"disasters", suite_disasters},
    {"memtiers", suite_memtiers},
    {"analysis", suite_analysis},
    {"profiles", suite_profiles},
};

} // namespace

// Phase 10 turned the world's features on by default (docs/phase10-plan.md
// C8). The suites predate that and test one feature at a time, so they run on
// the defaults from before it (Config::set_fallback); the ecology suite tests
// the new defaults themselves.
extern const char* const kLegacyDefaults; // (test_ecology.cpp clears and restores it)
const char* const kLegacyDefaults ="accel.mode = instant\n"
                                    "world.gpus = 0\n"
                                    "world.npus = 0\n"
                                    "cpu.wear = 0\n"
                                    "isa.table = nova_v1\n"
                                    "world.disk = 0\n"
                                    "disk.rot = 0.000001\n"
                                    "mem.volatility = 0\n"
                                    "salvage.ratio = 0\n"
                                    "signal.channels = 0\n"
                                    "node.endurance = 0\n"
                                    "cost.table = classic\n"
                                    "gpu.warps = false\n"
                                    "spark.gpu_grant = 0\n"
                                    "cpu.threads = false\n"
                                    "cpu.e_share = 0\n"
                                    "heat.ambient = 0\n"
                                    "hazard.rot_scale = 1\n"
                                    "mem.decay = 0\n"
                                    "disk.decay = 0\n"
                                    "vent.spacing = 0\n"
                                    "engulf.bite = false\n"
                                    "engulf.codons_per_tick = 16\n"
                                    "life.threads_end = false\n"
                                    "life.frozen_ticks = 0\n"
                                    "life.dormancy = false\n"
                                    "edge.corruption = 0\n"
                                    "edge.failure = 0\n"
                                    "world.land = 1\n"
                                    "world.drift = 0\n"
                                    // a varied world (docs/world-plan.md W9): substrates, sparks by activity
                                    // and the archive (disk tiers, W4) off
                                    "substrate.enabled = false\n"
                                    "spark.activity = 0\n"
                                    "world.archive = 0\n"
                                    "disaster.enabled = false\n" // docs/disasters-plan.md: suite disasters tests them
                                    "mem.fast_share = 0\n"      // memory tiers (W3): suite memtiers tests them
                                    // behaviour profiles (docs/analysis-plan.md A3): observe-only, so off
                                    // changes no hash; suite profiles turns them on
                                    "profile.enabled = false\n"
                                    // the live novelty meter (docs/analysis-plan.md A2): an observer, off here;
                                    // suites view and viewer turn it on
                                    "novelty.enabled = false\n";

int main(int argc, char** argv) {
    nova::Config::ParseError pe;
    if (!nova::Config::set_fallback(kLegacyDefaults, &pe)) {
        fprintf(stderr, "legacy defaults: line %u: %s\n", pe.line, pe.msg);
        return 2;
    }
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
