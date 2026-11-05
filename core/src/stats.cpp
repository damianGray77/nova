#include "nova/stats.h"

namespace nova {

void csv_field(TextBuf& out, Str s) {
    bool quote = false;
    for (usize i = 0; i < s.n; ++i) {
        char c = s.p[i];
        if (c == ',' || c == '"' || c == '\r' || c == '\n')
            quote = true;
    }
    if (!quote) {
        out.append(s);
        return;
    }
    out.append_char('"');
    for (usize i = 0; i < s.n; ++i) {
        if (s.p[i] == '"')
            out.append_char('"');
        out.append_char(s.p[i]);
    }
    out.append_char('"');
}

bool StatsCsv::open(const char* path, u32 window) {
    if (window == 0 || !file_.open(path))
        return false;
    window_ = window;
    ticks_ = 0;
    sum_ = {};
    file_.write("tick_first,tick_last,ticks,parcels_sent,parcels_refused,parcels_delivered,"
                "units_accepted,units_bounced,mutations,units_total,nodes_full,nodes_empty\n");
    return true;
}

void StatsCsv::record(const World& w) {
    if (!is_open())
        return;
    if (ticks_ == 0)
        first_tick_ = w.tick() - 1; // record() runs after the tick completed
    const TickStats& t = w.last_tick();
    sum_.parcels_sent += t.parcels_sent;
    sum_.parcels_refused += t.parcels_refused;
    sum_.parcels_delivered += t.parcels_delivered;
    sum_.units_accepted += t.units_accepted;
    sum_.units_bounced += t.units_bounced;
    sum_.mutations += t.mutations;
    if (++ticks_ == window_)
        write_row(w);
}

void StatsCsv::finish(const World& w) {
    if (!is_open())
        return;
    if (ticks_ > 0)
        write_row(w);
    file_.close();
}

void StatsCsv::write_row(const World& w) {
    u64 full = 0, empty = 0;
    for (u32 n = 0; n < w.node_count(); ++n) {
        if (w.pool(n) >= w.pool_capacity())
            ++full;
        if (w.pool(n) == 0)
            ++empty;
    }
    const u64 cols[] = {first_tick_,
                        first_tick_ + ticks_ - 1,
                        ticks_,
                        sum_.parcels_sent,
                        sum_.parcels_refused,
                        sum_.parcels_delivered,
                        sum_.units_accepted,
                        sum_.units_bounced,
                        sum_.mutations,
                        w.total_units(),
                        full,
                        empty};
    TextBuf row;
    for (usize i = 0; i < sizeof cols / sizeof cols[0]; ++i) {
        if (i)
            row.append_char(',');
        row.append_u64(cols[i]);
    }
    row.append_char('\n');
    file_.write(row.view());
    file_.flush();
    ticks_ = 0;
    sum_ = {};
}

} // namespace nova
