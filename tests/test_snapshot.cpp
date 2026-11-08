#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/snapshot.h"

using namespace nova;

namespace {

bool contains(TextBuf& t, const char* needle) {
    return strstr(t.c_str(), needle) != nullptr;
}

void test_round_trip() {
    Run a;
    CHECK(make_run(a, kBusyWorld, 4));
    a.run_until(120);
    ByteWriter bytes;
    a.snapshot(bytes);

    SnapshotHeader h;
    TextBuf err;
    CHECK(snapshot_parse(bytes.data(), bytes.size(), &h, &err));
    CHECK(h.version == kSnapshotVersion && h.seed == 42 && h.tick == 120);

    Run b;
    CHECK(b.init_from_snapshot(bytes.data(), bytes.size(), 20, &err));
    CHECK(b.world().tick() == 120);
    CHECK_EQ_U64(b.world().hash(), a.world().hash());
}

// Running straight through must equal stop -> snapshot -> resume, at different
// thread counts, with a knob change before the snapshot (so the snapshot must
// carry the effective config, not the starting one).
void test_resume_equivalence() {
    TextBuf err;
    Run straight;
    CHECK(make_run(straight, kBusyWorld, 1));
    CHECK(straight.schedule_knob(100, "demo.send_rate", "0.95", &err));
    straight.run_until(300);

    Run first;
    CHECK(make_run(first, kBusyWorld, 20));
    CHECK(first.schedule_knob(100, "demo.send_rate", "0.95", &err));
    first.run_until(150);
    ByteWriter bytes;
    first.snapshot(bytes);

    Run resumed;
    CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
    CHECK(resumed.config().get_str("demo.send_rate", "").eq("0.95"));
    resumed.run_until(300);
    CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());

    // Control: resuming without the knob in effect would differ.
    Run no_knob;
    CHECK(make_run(no_knob, kBusyWorld, 4));
    no_knob.run_until(300);
    CHECK(no_knob.world().hash() != straight.world().hash());
}

// A world whose initial sparks overfill every soup, so building it notes "no
// room" in each allocator's cache before anything is freed. A resumed world is
// built the same way and then loaded, so the note must not survive the load:
// kept, it refused the resumed run allocations the straight run made (the
// landscape's resume divergence, 2026-11-07).
void test_resume_after_full_start() {
    const char* const cfg = "seed = 42\n"
                            "world.model = soup\n"
                            "world.nodes = 8\n"
                            "world.chords = 4\n"
                            "world.soup_size = 2048\n"
                            "world.millicores = 400\n"
                            "cpu.instr_per_core = 2500\n"
                            "spark.initial = 300\n"
                            "spark.rate = 0.5\n"
                            "spark.grant = 1\n"
                            "spark.min_len = 8\n"
                            "spark.max_len = 48\n"
                            "seed.genome = SENSE ALLOC NOP1 COPY DJNZ NOP0 INC DIVIDE\n"
                            "seed.at = 1000\n"
                            "seed.millicores = 200\n";
    const u64 at[3] = {1, 10, 40};
    for (u32 i = 0; i < 3; ++i) {
        Run straight;
        CHECK(make_run(straight, cfg, 4));
        straight.run_until(at[i] + 20);
        Run first;
        CHECK(make_run(first, cfg, 4));
        first.run_until(at[i]);
        ByteWriter bytes;
        first.snapshot(bytes);
        Run resumed;
        TextBuf err;
        CHECK(resumed.init_from_snapshot(bytes.data(), bytes.size(), 4, &err));
        resumed.run_until(at[i] + 20);
        CHECK_EQ_U64(resumed.world().hash(), straight.world().hash());
    }
}

void test_rejects_damage() {
    Run a;
    CHECK(make_run(a, kBusyWorld, 1));
    a.run_until(10);
    ByteWriter w;
    a.snapshot(w);
    Array<u8> good;
    good.assign(w.data(), w.size());

    Array<u8> bad;
    TextBuf err;
    Run r1;

    bad.assign(good.data(), good.size());
    bad[good.size() / 2] ^= 0x01; // one flipped bit in the world section
    CHECK(!r1.init_from_snapshot(bad.data(), bad.size(), 1, &err));
    CHECK(contains(err, "checksum"));

    err.clear();
    Run r2;
    CHECK(!r2.init_from_snapshot(good.data(), good.size() - 1, 1, &err)); // truncated
    CHECK(err.size() > 0);

    err.clear();
    Run r3;
    bad.assign(good.data(), good.size());
    bad[0] = 'X';
    CHECK(!r3.init_from_snapshot(bad.data(), bad.size(), 1, &err));
    CHECK(contains(err, "bad magic"));

    err.clear();
    Run r4;
    bad.assign(good.data(), good.size());
    bad[8] = static_cast<u8>(kSnapshotVersion + 1); // a format from the future
    CHECK(!r4.init_from_snapshot(bad.data(), bad.size(), 1, &err));
    TextBuf future; // (whatever the current version is: 5 since the varied world's v4)
    future.append("unsupported format version ");
    future.append_u64(kSnapshotVersion + 1);
    CHECK(contains(err, future.c_str()));

    err.clear();
    Run r5;
    CHECK(!r5.init_from_snapshot(good.data(), 4, 1, &err));
}

void test_file_round_trip() {
    Run a;
    CHECK(make_run(a, kBusyWorld, 4));
    a.run_until(40);
    ByteWriter w;
    a.snapshot(w);
    const char* path = "nova_test_snapshot.snap";
    CHECK(write_file(path, w.data(), w.size()));
    Array<u8> back;
    CHECK(read_file(path, &back));
    CHECK(back.size() == w.size() && memcmp(back.data(), w.data(), w.size()) == 0);
    Run b;
    TextBuf err;
    CHECK(b.init_from_snapshot(back.data(), back.size(), 2, &err));
    CHECK_EQ_U64(b.world().hash(), a.world().hash());
    remove(path);
}

} // namespace

void suite_snapshot() {
    test_round_trip();
    test_resume_equivalence();
    test_resume_after_full_start();
    test_rejects_damage();
    test_file_round_trip();
}
