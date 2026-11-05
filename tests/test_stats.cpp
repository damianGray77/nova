#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/stats.h"

using namespace nova;

namespace {

void test_csv_field() {
    TextBuf t;
    csv_field(t, "plain");
    t.append_char('|');
    csv_field(t, "a,b");
    t.append_char('|');
    csv_field(t, "say \"hi\"");
    t.append_char('|');
    csv_field(t, "two\nlines");
    t.append_char('|');
    csv_field(t, "");
    CHECK(t.view().eq("plain|\"a,b\"|\"say \"\"hi\"\"\"|\"two\nlines\"|"));
}

// Splits text into lines (without the '\n').
usize split_lines(Str text, Str* out, usize max) {
    usize n = 0, i = 0;
    while (i < text.n && n < max) {
        usize e = i;
        while (e < text.n && text.p[e] != '\n')
            ++e;
        out[n++] = text.sub(i, e - i);
        i = e + 1;
    }
    return n;
}

void test_windows() {
    const char* path = "nova_test_stats.csv";
    Run run;
    CHECK(make_run(run, kBusyWorld, 4));
    TextBuf err;
    CHECK(run.open_stats(path, 4, &err));
    run.run_until(10);
    run.finish(); // writes the partial 2-tick window

    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    Str text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Str lines[8];
    usize n = split_lines(text, lines, 8);
    CHECK(n == 4); // header + rows for ticks 0-3, 4-7, 8-9
    CHECK(lines[0].starts_with("tick_first,tick_last,ticks,parcels_sent"));
    CHECK(lines[1].starts_with("0,3,4,"));
    CHECK(lines[2].starts_with("4,7,4,"));
    CHECK(lines[3].starts_with("8,9,2,"));

    // units_total (the 10th of 12 fields) is conserved: 200 nodes x 600 units.
    bool conserved = true;
    for (usize i = 1; i < n; ++i) {
        Str row = lines[i];
        usize field = 0, start = 0;
        for (usize c = 0; c <= row.n; ++c) {
            if (c == row.n || row.p[c] == ',') {
                if (field == 9) {
                    u64 v = 0;
                    conserved =
                        conserved && parse_u64(row.sub(start, c - start), &v) && v == 200ull * 600;
                }
                ++field;
                start = c + 1;
            }
        }
        conserved = conserved && field == 12;
    }
    CHECK(conserved);
    remove(path);
}

} // namespace

void suite_stats() {
    test_csv_field();
    test_windows();
}
