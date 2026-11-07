#pragma once

// The setup page's check (docs/config-plan.md C4): a configuration read with
// the engine's own code -- the budgets (budget.h), then the same parameter
// builders a run uses (World::check_settings, which reads every setting as
// World::init does) -- without building the world. It gives:
//
//   - every range or cross-check problem, by setting, as the engine words it
//     ("spark.min_len: must not exceed spark.max_len");
//   - the value each setting will actually have, computed defaults worked out
//     (the registry's value hook, nova/keys.h);
//   - the memory and disk the world will need, from the budgets' estimates
//     (the sums resolve_budgets' fit check makes), against what the machine
//     has;
//   - for a landscape, what it will be: nova land's summary.
//
// The config's first problem is all Config keeps, so the check reads again
// with each problem's setting left out (back to its default) until the reads
// are clean, collecting one problem per setting. Not thread-safe: it sets the
// registry's process-wide value hook while it reads (the page runs it on the
// UI thread, where nothing else reads settings).
#include "nova/array.h"
#include "nova/config.h"
#include "nova/landscape.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

struct SettingProblem {
    char key[64];   // the setting it belongs to; "" for the file as a whole
    char what[256]; // what is wrong, as the engine says it
};

// A landscape's summary (nova land's report).
struct LandSummary {
    bool built = false; // generated (if not, why is among the problems, at world.nodes)
    double width = 1, land_share = 0, spacing = 0, ms = 0;
    u32 raster_w = 0, raster_h = 0;
    u32 nodes = 0, masses = 0, peopled = 0; // landmasses, and those with nodes
    u32 largest[8] = {};                    // the largest landmasses' node counts
    u32 hist[14] = {};                      // nodes by land paths (the last: 13 or more)
    double paths_mean = 0;                  // land paths per node
    u32 hermits = 0, hubs = 0, long_links = 0, bridges = 0;
    u32 open_every = 0, open_ticks = 0;
    // Substrates (docs/world-plan.md W2), as nova land reports them: whether
    // they're on, and how many distinct ones (each node's eight levels
    // together) the nodes have.
    bool substrates = false;
    u32 substrates_distinct = 0;
};

struct SettingsCheck {
    Array<SettingProblem> problems;
    // Per registry entry (key_at order): whether the run reads it (0: its
    // feature is off), and the value it reads, as config text.
    Array<u8> read;
    Array<u32> val_off, val_len;
    Array<char> text;
    Str effective(usize key) const {
        return key < read.size() && read[key] ? Str(text.data() + val_off[key], val_len[key]) : Str();
    }
    // The world's size against the machine (budget.h, C9): RAM for the soups
    // (and the disk tier when it is in RAM), the disk tier's files -- the fit
    // check's own sums (world_footprint), the archive (W4) included.
    bool soup = false, disk_in_file = false;
    u64 nodes = 0, soup_size = 0, disk = 0, archive = 0; // disk, archive: codons a node
    double ram = 0, ram_avail = 0, disk_file = 0, disk_free = 0;
    TextBuf notes; // what the budgets resolved to, a line each
    bool landscape = false;
    LandSummary land;
    // The landscape is generated again only when its settings change (its
    // own, or the substrates', whose count the summary gives).
    bool land_cached = false;
    u64 land_seed = 0;
    LandParams land_params;
    bool land_subs = false;
    SubstrateParams land_sub_params;
    TextBuf land_why; // why it couldn't be generated
};

// What a front end reads itself before the world (app's read_run_keys: run.*,
// stats.*, output.*), so the check reads every setting a run does.
using FrontEndReader = void (*)(Config& cfg);

// Checks a configuration given as config text (`key = value` lines). With
// `machine`, the budgets are resolved and the world's fit checked against this
// machine now (a world about to start; a running world's own memory would
// count against it). `out` may be reused: its landscape stays cached.
void check_settings(Str config_text, SettingsCheck* out, FrontEndReader front_end, bool machine);

} // namespace nova
