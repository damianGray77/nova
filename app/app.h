#pragma once

// What the command-line front end's files share: `nova run` and `nova view`
// read the same run keys and open the same outputs, so a watched run is a
// real run (Phase 6 plan C3).
#include "nova/config.h"
#include "nova/run.h"
#include "nova/str.h"

namespace nova {

// The run.*, stats.* and output.* keys (paths copied: config views die on
// knob changes).
struct RunKeys {
    u64 ticks = 1000;   // run.ticks (0 = no limit)
    u64 threads = 0;    // run.threads
    double seconds = 0; // run.seconds
    i64 after = -1;     // run.after_replicator
    u64 window = 100;   // stats.window
    u64 every = 100;    // replay.checkpoint_every
    u64 snapshot_every = 0; // output.snapshot_every (Phase 6)
    u64 nodes_every = 1;    // stats.nodes_every (Phase 7)
    bool profile = false;   // --profile (Phase 7)
    TextBuf budget_threads; // budget.threads: all | all_but_one (default) | N | P% (Phase 7b)
    TextBuf priority;       // budget.priority: below_normal (default) | normal | idle
    TextBuf csv, nodes, signals, replay, snap, bank, lin, result;
    TextBuf prof, arr; // output.profiles, output.arrivals (behaviour profiles, A3)
};

// Reads the keys (marking them used); range problems go to cfg.fail.
void read_run_keys(Config& cfg, RunKeys* k);
// The threads a run uses (Phase 7b): an explicit count (run.threads or
// --threads) wins, else budget.threads (default every logical processor but
// one, so the machine stays responsive). Sets the process priority from
// budget.priority (default below normal). Prints what budgets resolved to.
u32 machine_threads(const RunKeys& k, u64 explicit_threads);
void apply_priority(const RunKeys& k);
void print_budget_notes(const Run& run);
// Opens every output the keys name, after Run::init; prints why and returns
// false on failure.
bool open_outputs(Run& run, RunKeys& k);
// One --set key=value; prints why and returns false if malformed.
bool apply_set(Config& cfg, const char* kv);
// Creates the parent directory of `path` (one level) if it is missing.
void ensure_parent_dir(const char* path);
// result.txt (see main.cpp).
bool write_result(const char* path, const Run& run, const char* stop, double seconds);
// The end of a run: result.txt and the final snapshot, as the keys say.
bool finish_outputs(const Run& run, RunKeys& k, const char* stop, double seconds);
double now_ms();

// nova view (view_cmd.cpp).
int cmd_view(int argc, char** argv);

} // namespace nova
