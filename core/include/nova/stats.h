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
// Soup model columns instead (sums over every node; the last four are taken at
// the window's end):
//   tick_first, tick_last, ticks
//   instructions, births, deaths, sparks, halts
//   copy_errors, flaws, rot_flips     hazards
//   cpu_released, mem_released        millicores / codons freed by heat
//   engulf_tries, engulfs, inject_tries, injects
//   bodies, cpus, free_millicores, waste
#include "nova/io.h"
#include "nova/str.h"
#include "nova/world.h"

namespace nova {

// Appends s as one CSV field, quoting it if it holds a comma, quote, CR or LF.
void csv_field(TextBuf& out, Str s);

class StatsCsv {
  public:
    bool open(const char* path, u32 window, Model model = Model::demo); // writes the header
    bool is_open() const { return file_.is_open(); }

    void record(const World& w); // call once after every tick
    void finish(const World& w); // writes a final partial row, then closes

  private:
    void write_row(const World& w);

    TextFile file_;
    u32 window_ = 0;
    u32 ticks_ = 0;
    u64 first_tick_ = 0;
    TickStats sum_ = {};
    Model model_ = Model::demo;
    SoupTally soup_sum_ = {};
};

} // namespace nova
