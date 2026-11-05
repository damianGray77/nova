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

bool StatsCsv::open(const char* path, u32 window, Model model) {
    if (window == 0 || !file_.open(path))
        return false;
    window_ = window;
    ticks_ = 0;
    sum_ = {};
    soup_sum_ = {};
    model_ = model;
    if (model == Model::soup) {
        file_.write("tick_first,tick_last,ticks,instructions,births,deaths,sparks,halts,"
                    "copy_errors,flaws,rot_flips,cpu_released,mem_released,engulf_tries,engulfs,"
                    "inject_tries,injects,bodies,cpus,free_millicores,waste\n");
        return true;
    }
    file_.write("tick_first,tick_last,ticks,parcels_sent,parcels_refused,parcels_delivered,"
                "units_accepted,units_bounced,mutations,units_total,nodes_full,nodes_empty\n");
    return true;
}

void StatsCsv::record(const World& w) {
    if (!is_open())
        return;
    if (ticks_ == 0)
        first_tick_ = w.tick() - 1; // record() runs after the tick completed
    if (model_ == Model::soup) {
        u64* sum = reinterpret_cast<u64*>(&soup_sum_);
        const u64* t = reinterpret_cast<const u64*>(&w.last_soup_tick());
        for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
            sum[i] += t[i];
        if (++ticks_ == window_)
            write_row(w);
        return;
    }
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
    if (model_ == Model::soup) {
        u64 bodies = 0, cpus = 0, free_units = 0, waste = 0;
        for (u32 n = 0; n < w.node_count(); ++n) {
            const Soup& s = w.node_soup(n);
            bodies += s.body_count();
            for (usize k = 0; k < s.body_count(); ++k)
                cpus += s.body(s.body_at(k)).ncpu;
            free_units += s.free_units();
            waste += s.waste();
        }
        TextBuf row;
        row.append_u64(first_tick_);
        row.append_char(',');
        row.append_u64(first_tick_ + ticks_ - 1);
        row.append_char(',');
        row.append_u64(ticks_);
        const u64* t = reinterpret_cast<const u64*>(&soup_sum_);
        for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i) {
            row.append_char(',');
            row.append_u64(t[i]);
        }
        const u64 tail[] = {bodies, cpus, free_units, waste};
        for (u64 v : tail) {
            row.append_char(',');
            row.append_u64(v);
        }
        row.append_char('\n');
        file_.write(row.view());
        file_.flush();
        ticks_ = 0;
        soup_sum_ = {};
        return;
    }
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
