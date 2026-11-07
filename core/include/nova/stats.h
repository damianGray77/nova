#pragma once

// CSV stats, one row per window of ticks (design.md 7.1 B/H: CSV opens in
// Excel, so no analysis tool is needed until Nova's own UI exists).
//
// Columns:
//   tick_first, tick_last, ticks      the ticks this row covers
//   parcels_sent, parcels_refused     summed over the window
//   parcels_delivered, units_accepted, units_bounced, mutations
//   units_total                       sum of all pools at window end (conserved)
//   nodes_full, nodes_empty           pools at capacity / at zero, at window end
//
// Soup model columns instead (sums over every node; from `bodies` on they are
// taken at the window's end):
//   tick_first, tick_last, ticks
//   instructions, births, deaths, sparks, halts
//   copy_errors, flaws, rot_flips     hazards
//   cpu_released, mem_released        millicores / codons freed by heat
//   engulf_tries, engulfs, inject_tries, injects
//   upkeep                            instructions charged as memory upkeep (1b)
//   cpu_worn                          millicores returned by CPU wear (1b)
//   gpu_elements, gpu_released, gpu_worn   GPU element-ops done, units freed
//                                     by heat, units returned by wear (2)
//   npu_macs, npu_released, npu_worn  NPU multiply-accumulates done, units
//                                     freed by heat, units returned by wear (2b)
//   sent, arrived, lost               daughters dispersed to a neighbour, placed
//                                     on arrival, lost on arrival for want of
//                                     room (3)
//   corrupted, transit_lost           codons flipped in transit; cargoes lost on
//                                     a failed or vanished link (4)
//   disk_stores, disk_loads, disk_compares, disk_rot   DISK ops; disk rot flips (4)
//   wipes, wiped, wipe_deaths         volatility wipes, codons wiped, bodies
//                                     wiped whole (4)
//   emits, listens                    signal ops (4)
//   collapses, recoveries             nodes collapsed; sites become nodes (4)
//   garbled                           codons the salvage rule replaced (4)
//   bodies, cpus, free_millicores, waste
//   gpu_held, npu_held                GPU and NPU units held by bodies (2, 2b)
//   in_flight                         cargoes in transit (3)
// and the replicator detector's view (Phase 1b; zeros when detection is off):
//   links                             inheritance links during the window
//   inject_links                      ... of which made by INJECT
//   gpu_links                         ... and DIVIDE links copied mostly by the GPU (2)
//   max_depth                         deepest living lineage member
//   lineages                          lineages with a living member
//   top_lineage                       living members of the largest lineage
//   genotypes_in_top                  distinct genotypes among them
//   colonised                         nodes holding a living member of a lineage
//                                     founded on another node (3)
//   links_down, disk_held, live_nodes links down now; disk codons held; nodes
//                                     live now (4)
//   warp_steps, lane_instr            warp steps run; lane instructions run (8)
//   warp_host, launches, gpu_births   host instructions done for warps; warps
//                                     launched; bodies born on a GPU (8)
//   warps, gpu_bodies                 warps alive; bodies born on a GPU alive (8)
//   actions_started, actions_landed,  ENGULFs, INJECTs and long instructions
//   actions_failed, actions_defended  begun as actions; landed and failed at
//                                     their end; failures because the target
//                                     changed in time (9, timed cost tables)
//   threads_started, e_work, e_held   threads programs started; work done on E
//                                     millicores; E millicores held (9c)
//   vent_codons, decayed              codons vents wrote; bits decayed out of dead
//                                     matter (10)
//   bites_waste, bites_body,          ENGULF bites of raw material and of pieces of
//   codons_bitten                     bigger bodies; codons bitten (10)
//   threads_ended                     halted threads that left their bodies (10)
//   died_halted, died_frozen,         deaths by cause: no running thread, frozen,
//   died_worn                         no millicore left (10)
//   dormant_started, dormant_woke,    dormancy and drift (docs/dormancy-plan.md):
//   dormant_expired, dormant_eaten,   frozen bodies gone dormant; woken where they
//   dormant_hops                      rested; dead with their reserves spent;
//                                     swallowed while resting; hops taken
//   drifting, resting                 dormant programs in transit; resting on nodes
//   compress                         soup compressibility (compress.h), mean
//                                     over nodes: ~1 random, lower = repetitive
//   ambient                           the ambient heat input, mean over nodes (4)
#include "nova/io.h"
#include "nova/str.h"
#include "nova/world.h"

namespace nova {

// Appends s as one CSV field, quoting it if it holds a comma, quote, CR or LF.
void csv_field(TextBuf& out, Str s);

// The soup model's stats columns (the list above), shared by stats.csv and the
// viewer's charts (Phase 6) so both show exactly the same numbers.
inline constexpr u32 kStatsColumns = 92;
inline constexpr u32 kStatsIntColumns = 90; // tick_first .. live_nodes, Phase 8's warp, Phase 9's action,
                                            // Phase 9c's thread, Phase 10's and dormancy's columns; then
                                            // compress, ambient
const char* stats_column(u32 i);
// Index of a column by name, or kStatsColumns if there is none.
u32 stats_column_index(Str name);

struct StatsRow {
    u64 v[kStatsIntColumns];
    double compress, ambient;
    double value(u32 i) const {
        return i < kStatsIntColumns ? static_cast<double>(v[i]) : (i == kStatsIntColumns ? compress : ambient);
    }
};
// The detector's link totals at the previous row (each row shows the change).
struct StatsMarks {
    u64 links = 0, inject = 0, gpu = 0;
};
// One soup-model row: `sum` is the window's summed tallies; the rest is the
// world at the window's end. Updates *marks. `compress` false skips the
// compressibility estimate (it reads every codon) and leaves it 0.
void stats_soup_row(const World& w, const SoupTally& sum, u64 first_tick, u32 ticks, StatsMarks* marks, bool compress,
                    StatsRow* out);

// The per-node file (Phase 3, output.nodes; soup model): one row per node per
// window, at the window's end:
//   tick_first, tick_last, node
//   x, y, degree                      hidden position (0 in a ring world), neighbours
//   soup_size, millicores, gpus, npus, cool, spark_rate, radiation
//                                     the node's properties (radiation: its hazard
//                                     rates over the configured ones)
//   bodies, waste, free_millicores, free_gpus, free_npus
//   births, deaths, sent, arrived, lost   summed over the window
//   lineages, colonists               lineages living here; members of lineages
//                                     founded elsewhere
//   live, health, life, born          the lifecycle (Phase 4): a node now or
//                                     fallow, health 0-100, lives after the
//                                     first, the tick it last appeared
class StatsCsv {
  public:
    bool open(const char* path, u32 window, Model model = Model::demo); // writes the header
    // After open(), soup model only. `every` (stats.nodes_every, Phase 7):
    // rows every Nth window, each covering every tick since the last rows.
    bool open_nodes(const char* path, u32 every = 1);
    // The researcher's signal tap (Phase 4, output.signals), after open():
    // per window and channel, emissions, total |amplitude| and the mean
    // |field| heard per node per tick.
    bool open_signals(const char* path, u32 channels);
    bool is_open() const { return file_.is_open(); }

    void record(const World& w); // call once after every tick
    void finish(const World& w); // writes a final partial row, then closes

  private:
    void write_row(const World& w, bool final);
    void write_node_rows(const World& w);

    TextFile file_;
    TextFile nodes_;
    Array<u64> node_sum_; // per node: births, deaths, sent, arrived, lost
    u32 nodes_every_ = 1, node_windows_ = 0;
    u64 node_first_ = 0; // first tick the next node rows cover
    u64 last_tick_ = 0;  // last tick of the latest whole window
    TextFile signals_;
    u32 channels_ = 0;
    Array<u64> sig_emits_, sig_amp_;
    Array<double> sig_field_; // sum over ticks of the mean |field| per node
    u32 window_ = 0;
    u32 ticks_ = 0;
    u64 first_tick_ = 0;
    TickStats sum_ = {};
    Model model_ = Model::demo;
    SoupTally soup_sum_ = {};
    StatsMarks marks_; // detector links counted at the last row
};

} // namespace nova
