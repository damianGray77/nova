#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/keys.h"
#include "nova/replay.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

const char* const kLogPath = "nova_test_run.replay";

// Records a 300-tick run at 20 threads with two knob changes and checkpoints
// every 50 ticks. Returns the final hash.
u64 record_run(const char* path, bool with_knobs) {
    Run run;
    if (!make_run(run, kBusyWorld, 20))
        return 0;
    TextBuf err;
    if (with_knobs) {
        run.schedule_knob(200, "demo.mutations_per_tick", "100", &err);
        run.schedule_knob(100, "demo.send_rate", "0.9", &err); // out of order on purpose
    }
    if (path && !run.open_replay(path, 50, &err))
        return 0;
    run.run_until(300);
    run.finish();
    return run.world().hash();
}

Str file_text(Array<u8>& bytes) {
    return Str(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void test_log_and_verify() {
    u64 final_hash = record_run(kLogPath, true);
    CHECK(final_hash != 0);

    ReplayLog log;
    TextBuf err;
    CHECK(log.load_file(kLogPath, &err));
    CHECK(log.version() == kReplayVersion && kReplayVersion == 6); // new logs are v6 (docs/bites-plan.md)
    CHECK(log.knob_count() == 2);
    CHECK(log.knob_tick(0) == 100 && log.knob_key(0).eq("demo.send_rate") &&
          log.knob_value(0).eq("0.9"));
    CHECK(log.knob_tick(1) == 200 && log.knob_key(1).eq("demo.mutations_per_tick"));
    CHECK(log.checkpoints().size() == 7); // ticks 0, 50, ..., 300
    CHECK(log.has_end() && log.end().tick == 300 && log.end().hash == final_hash);

    // The logged config is the tick-0 config: knob values appear only as events.
    Config c;
    Config::ParseError pe;
    CHECK(c.parse(log.config_text(), &pe));
    CHECK(c.get_str("demo.send_rate", "").eq("0.7"));

    ReplayResult r;
    CHECK(replay_verify(log, 1, &r)); // recorded at 20 threads, replayed at 1
    CHECK(r.ok && r.checkpoints_checked == 8);
    CHECK(replay_verify(log, 4, &r));

    // The knobs matter: the same run without them ends elsewhere.
    CHECK(record_run(nullptr, false) != final_hash);
}

void test_detects_tampering() {
    Array<u8> bytes;
    CHECK(read_file(kLogPath, &bytes));

    // 1. Change a logged knob value: replay must diverge at the first
    //    checkpoint after the knob takes effect (knob at 100 -> checkpoint 150).
    {
        TextBuf t;
        t.append(file_text(bytes));
        char* p = strstr(const_cast<char*>(t.c_str()), "demo.send_rate = 0.9");
        CHECK(p != nullptr);
        if (p)
            p[strlen("demo.send_rate = 0.")] = '8';
        ReplayLog log;
        TextBuf err;
        CHECK(log.parse(t.view(), &err));
        ReplayResult r;
        CHECK(!replay_verify(log, 4, &r));
        CHECK(!r.ok && r.diverged_at == 150);
        CHECK(strstr(r.msg, "diverged at tick 150") != nullptr);
    }
    // 2. Corrupt a checkpoint hash.
    {
        TextBuf t;
        t.append(file_text(bytes));
        char* p = strstr(const_cast<char*>(t.c_str()), "\nhash 250 ");
        CHECK(p != nullptr);
        if (p)
            p[10] = p[10] == '0' ? '1' : '0';
        ReplayLog log;
        TextBuf err;
        CHECK(log.parse(t.view(), &err));
        ReplayResult r;
        CHECK(!replay_verify(log, 1, &r));
        CHECK(r.diverged_at == 250);
    }
    remove(kLogPath);
}

void test_parse_errors() {
    ReplayLog log;
    TextBuf err;
    CHECK(!log.parse("", &err));
    err.clear();
    CHECK(!log.parse("not a log\n", &err));
    err.clear();
    CHECK(!log.parse("nova-replay 7\n", &err));
    CHECK(strstr(err.c_str(), "unsupported replay version 7") != nullptr);
    err.clear();
    CHECK(log.parse("nova-replay 6\nevent 5 replicator node=0\n", &err) && log.version() == 6);
    err.clear();
    // Logs before v6 come from an incompatible engine (docs/bites-plan.md).
    CHECK(!log.parse("nova-replay 5\nevent 5 replicator node=0\n", &err));
    CHECK(strstr(err.c_str(), "unsupported replay version 5") != nullptr);
    err.clear();
    CHECK(log.parse("nova-replay 6\nevent 5 replicator node=0 x=1\nevent 5 b\nevent 7 c\n", &err));
    CHECK(log.version() == 6 && log.event_count() == 3 && log.event_tick(2) == 7);
    CHECK(log.event_text(0).eq("replicator node=0 x=1"));
    err.clear();
    CHECK(!log.parse("nova-replay 6\nevent 7 a\nevent 5 b\n", &err)); // out of order
    err.clear();
    CHECK(!log.parse("nova-replay 6\nevent 7\n", &err)); // no text
    err.clear();
    CHECK(!log.parse("nova-replay 6\nbogus 1 2\n", &err));
    CHECK(strstr(err.c_str(), "line 2") != nullptr);
    err.clear();
    CHECK(!log.parse("nova-replay 6\nhash 5 0123\n", &err)); // short hex
    err.clear();
    CHECK(!log.parse("nova-replay 6\nend 5 0000000000000000\nhash 6 0000000000000000\n", &err));
    err.clear();
    CHECK(!log.parse("nova-replay 6\nhash 6 0000000000000000\nhash 5 0000000000000000\n", &err));
    err.clear();
    CHECK(log.parse("# comment\nnova-replay 6\nconfig seed = 1\n\n", &err));
    CHECK(log.config_text().eq("seed = 1\n"));
}

void test_knob_scheduling_rules() {
    Run run;
    CHECK(make_run(run, kBusyWorld, 1));
    run.run_until(10);
    TextBuf err;
    CHECK(!run.schedule_knob(5, "demo.send_rate", "0.1", &err));
    CHECK(strstr(err.c_str(), "in the past") != nullptr);
    err.clear();
    CHECK(!run.schedule_knob(20, "world.nodes", "5", &err));
    CHECK(strstr(err.c_str(), "structural") != nullptr);
    err.clear();
    CHECK(!run.schedule_knob(20, "demo.send_rate", "7", &err));
    err.clear();
    CHECK(run.schedule_knob(10, "demo.send_rate", "0", &err)); // current tick is allowed
    run.step();
    CHECK(run.world().last_tick().parcels_sent == 0);
    CHECK(run.config().get_str("demo.send_rate", "").eq("0"));
    err.clear();
    CHECK(!run.open_replay("nova_test_late.replay", 10, &err)); // not at tick 0
}

// A log with knob records (as the viewer's toolbar wrote them before a run's
// settings were fixed, docs/config-plan.md C3) replays to every logged hash,
// at any thread count; without its knobs it diverges; and a locked run (as
// nova run and nova view lock theirs) takes no new change. (The log recorded
// 2026-11-07 is a v2 file, refused since v6: this one is written afresh.)
void test_knob_log() {
    const char* path = "nova_test_knobs.replay";
    const char* cfg = "seed = 11\nworld.model = soup\nworld.nodes = 4\nworld.soup_size = 1024\n"
                      "world.millicores = 400\nspark.initial = 20\nspark.rate = 0.2\nspark.grant = 10\n"
                      "cpu.instr_per_core = 500\n";
    TextBuf err;
    u64 end = 0;
    {
        Run run;
        CHECK(make_run(run, cfg, 2));
        CHECK(run.open_replay(path, 30, &err));
        CHECK(run.schedule_knob(30, "spark.rate", "0.6", &err));
        CHECK(run.schedule_knob(60, "cpu.wear", "0.01", &err));
        CHECK(run.schedule_knob(90, "spark.grant", "25", &err));
        run.run_until(120);
        run.finish();
        end = run.world().hash();
    }
    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    remove(path);
    ReplayLog log;
    CHECK(log.parse(file_text(bytes), &err));
    CHECK(log.version() == kReplayVersion && log.knob_count() == 3 && log.checkpoints().size() == 5);
    ReplayResult r;
    CHECK(replay_verify(log, 1, &r));
    CHECK(r.ok && r.checkpoints_checked == 6);
    CHECK(replay_verify(log, 4, &r) && r.ok);
    CHECK_EQ_U64(log.end().hash, end);
    // The knobs mattered: without them the run diverges after the first.
    TextBuf bare;
    const Str all = file_text(bytes);
    usize at = 0;
    for (usize i = 0; i < all.n; ++i) {
        if (all.p[i] != '\n')
            continue;
        const Str line = all.sub(at, i - at + 1);
        if (!line.starts_with("knob "))
            bare.append(line);
        at = i + 1;
    }
    ReplayLog nolog;
    CHECK(nolog.parse(bare.view(), &err));
    CHECK(!replay_verify(nolog, 2, &r) && r.diverged_at == 60);
    // A locked run takes no new change.
    Run run;
    Config::ParseError pe;
    CHECK(run.config().parse(log.config_text(), &pe));
    CHECK(run.init(2, &err));
    run.lock_knobs();
    err.clear();
    CHECK(run.knobs_locked() && !run.schedule_knob(30, "spark.rate", "0.6", &err));
    CHECK(strstr(err.c_str(), "fixed from its start") != nullptr);
}

// Logs before v6 come from an incompatible engine (docs/bites-plan.md): refused.
void test_old_logs_refused() {
    ReplayLog log;
    TextBuf err;
    CHECK(!log.parse("nova-replay 5\nconfig seed = 1\nhash 0 0000000000000001\n", &err));
    CHECK(strstr(err.c_str(), "incompatible") != nullptr);
}

} // namespace

void suite_replay() {
    test_log_and_verify();
    test_detects_tampering();
    test_parse_errors();
    test_knob_scheduling_rules();
    test_knob_log();
    test_old_logs_refused();
}
