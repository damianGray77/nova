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
#include "nova/io.h"
#include "nova/str.h"
#include "nova/world.h"

namespace nova {

// Appends s as one CSV field, quoting it if it holds a comma, quote, CR or LF.
void csv_field(TextBuf& out, Str s);

class StatsCsv {
  public:
    bool open(const char* path, u32 window); // writes the header row
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
};

} // namespace nova
