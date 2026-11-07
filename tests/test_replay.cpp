#include <cstdio>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/keys.h"
#include "nova/legacy.h"
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
    CHECK(log.version() == kReplayVersion && kReplayVersion == 3); // new logs are v3 (legacy.h)
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
    CHECK(!log.parse("nova-replay 4\n", &err));
    CHECK(strstr(err.c_str(), "unsupported replay version 4") != nullptr);
    err.clear();
    CHECK(log.parse("nova-replay 3\nevent 5 replicator node=0\n", &err) && log.version() == 3);
    err.clear();
    // Events exist from version 2; v1 logs still load without them.
    CHECK(!log.parse("nova-replay 1\nevent 5 replicator node=0\n", &err));
    err.clear();
    CHECK(log.parse("nova-replay 2\nevent 5 replicator node=0 x=1\nevent 5 b\nevent 7 c\n", &err));
    CHECK(log.version() == 2 && log.event_count() == 3 && log.event_tick(2) == 7);
    CHECK(log.event_text(0).eq("replicator node=0 x=1"));
    err.clear();
    CHECK(!log.parse("nova-replay 2\nevent 7 a\nevent 5 b\n", &err)); // out of order
    err.clear();
    CHECK(!log.parse("nova-replay 2\nevent 7\n", &err)); // no text
    err.clear();
    CHECK(!log.parse("nova-replay 1\nbogus 1 2\n", &err));
    CHECK(strstr(err.c_str(), "line 2") != nullptr);
    err.clear();
    CHECK(!log.parse("nova-replay 1\nhash 5 0123\n", &err)); // short hex
    err.clear();
    CHECK(!log.parse("nova-replay 1\nend 5 0000000000000000\nhash 6 0000000000000000\n", &err));
    err.clear();
    CHECK(!log.parse("nova-replay 1\nhash 6 0000000000000000\nhash 5 0000000000000000\n", &err));
    err.clear();
    CHECK(log.parse("# comment\nnova-replay 1\nconfig seed = 1\n\n", &err));
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

// --- logs from before v3 (legacy.h; docs/results/integration.md) ------------------

// A log written by the build before the varied world (4daa357, Debug): `nova
// run` on this config, on that build's own defaults -- a procedural world of
// 12 nodes with a disk of 512 codons -- for 80 ticks. Its hashes are the
// world as that build ran it. On this build's defaults the same config has
// substrates, sparks by activity and an archive of 128 codons.
const char* const kOldDiskLog = "nova-replay 2\n"
                                "config seed = 11\n"
                                "config world.model = soup\n"
                                "config world.graph = procedural\n"
                                "config world.nodes = 12\n"
                                "config world.soup_size = 2048\n"
                                "config world.millicores = 600\n"
                                "config world.disk = 512\n"
                                "config spark.initial = 20\n"
                                "config spark.rate = 0.5\n"
                                "config accel.gpu = cpu\n"
                                "config accel.npu = cpu\n"
                                "config warp.device = cpu\n"
                                "config run.threads = 4\n"
                                "config run.ticks = 80\n"
                                "config replay.checkpoint_every = 20\n"
                                "config output.replay = replay_old.log\n"
                                "hash 0 7fdb46c43695b0e1\n"
                                "hash 20 62870c0ece2912fd\n"
                                "hash 40 63abeb0bd83c5932\n"
                                "hash 60 945400453d588e28\n"
                                "hash 80 57978c34d6bf41e8\n"
                                "end 80 57978c34d6bf41e8\n";

// The same world as a config for this build.
const char* const kDiskWorld = "seed = 11\nworld.model = soup\nworld.graph = procedural\nworld.nodes = 12\n"
                               "world.soup_size = 2048\nworld.millicores = 600\nworld.disk = 512\nspark.initial = 20\n"
                               "spark.rate = 0.5\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = cpu\n";
const char* const kNewFeaturesOff =
    "spark.activity = 0\nsubstrate.enabled = false\nworld.archive = 0\ndisaster.enabled = false\n";

// The logged config a parsed log gives to replay, as a Config.
bool replayed_config(Str text, Config* out) {
    ReplayLog log;
    TextBuf err;
    Config::ParseError pe;
    return log.parse(text, &err) && log.replay_config(out, &pe);
}

// What a log replays with: a log before v3 takes the legacy defaults for the
// settings it doesn't name, only where the world reads them; a v3 log doesn't.
void test_legacy_config() {
    // The text parses, and every line is a registry setting.
    Config legacy;
    Config::ParseError pe;
    CHECK(legacy.parse(kLegacyFormatDefaults, &pe) && legacy.count() >= 4);
    bool settings = true;
    for (usize i = 0; i < legacy.count(); ++i)
        settings = settings && key_find(legacy.key(i)) != nullptr;
    CHECK(settings);
    const usize L = legacy.count();
    Config c;
    CHECK(replayed_config("nova-replay 2\nconfig world.model = soup\nconfig world.graph = procedural\n", &c));
    CHECK(c.count() == 2 + L && c.get_str("spark.activity", "").eq("0") &&
          c.get_str("substrate.enabled", "").eq("false") && c.get_str("world.archive", "").eq("0") &&
          c.get_str("disaster.enabled", "").eq("false"));
    CHECK(replayed_config("nova-replay 1\nconfig world.model = soup\nconfig world.graph = landscape\n", &c));
    CHECK(c.count() == 2 + L); // v1 too
    CHECK(replayed_config("nova-replay 3\nconfig world.model = soup\nconfig world.graph = procedural\n", &c));
    CHECK(c.count() == 2 && !c.has("spark.activity") && !c.has("world.archive") && !c.has("disaster.enabled"));
    // A setting the log names keeps its value.
    CHECK(replayed_config("nova-replay 2\nconfig world.model = soup\nconfig world.archive = 256\n", &c));
    CHECK(c.get_str("world.archive", "").eq("256") && c.get_str("spark.activity", "").eq("0"));
    // A ring reads no substrate settings, and a demo world none of these.
    CHECK(replayed_config("nova-replay 2\nconfig world.model = soup\n", &c));
    CHECK(c.count() == L && !c.has("substrate.enabled"));
    CHECK(replayed_config("nova-replay 2\nconfig seed = 1\n", &c));
    CHECK(c.count() == 1);
}

// Rewrites a log as the build before v3 would have written it: header 2, and
// none of the given config lines (settings that build didn't have).
void as_v2(Str text, const char* drop, TextBuf* out) {
    usize i = 0;
    while (i < text.n) {
        usize e = i;
        while (e < text.n && text.p[e] != '\n')
            ++e;
        const Str line = text.sub(i, e - i);
        i = e + 1;
        if (line.starts_with("nova-replay ")) {
            out->append("nova-replay 2\n");
            continue;
        }
        bool skip = false;
        for (const char* d = drop; *d && !skip;) {
            const char* n = strchr(d, '\n');
            const Str setting(d, static_cast<usize>(n - d));
            TextBuf want;
            want.append("config ");
            want.append(setting);
            skip = line.eq(want.view());
            d = n + 1;
        }
        if (!skip) {
            out->append(line);
            out->append_char('\n');
        }
    }
}

// A log from before this build, of a world with a disk, replays to the hashes
// it recorded: the settings it doesn't name take the legacy defaults, not
// this build's (on which the same log diverges). On the engine's own defaults
// (no test fallback), where the new features are on.
void test_legacy_logs() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    // 1. The old build's own log.
    {
        ReplayLog log;
        TextBuf err;
        CHECK(log.parse(kOldDiskLog, &err) && log.version() == 2);
        ReplayResult r;
        CHECK(replay_verify(log, 4, &r));
        if (!r.ok)
            fprintf(stderr, "  old log: %s\n", r.msg);
        CHECK(r.ok && r.checkpoints_checked == 6);
        // Read as a v3 log, the same records replay on this build's defaults: another world.
        TextBuf v3;
        v3.append("nova-replay 3");
        v3.append(Str(kOldDiskLog + strlen("nova-replay 2")));
        ReplayLog now;
        CHECK(now.parse(v3.view(), &err) && now.version() == 3);
        ReplayResult r3;
        CHECK(!replay_verify(now, 4, &r3) && !r3.ok);
    }
    // 2. A v2 log made from this build: the world with the new features
    //    explicitly off (as the build before them ran it, and ends on the old
    //    build's hash), written by the replay writer, then rewritten as the
    //    old build wrote it (header 2, the three settings unnamed).
    {
        const char* path = "nova_test_legacy.replay";
        TextBuf cfg;
        cfg.append(kDiskWorld);
        cfg.append(kNewFeaturesOff);
        Run run;
        TextBuf err;
        CHECK(make_run(run, cfg.c_str(), 20));
        CHECK(run.open_replay(path, 20, &err));
        run.run_until(80);
        run.finish();
        CHECK_EQ_U64(run.world().hash(), 0x57978c34d6bf41e8ull); // the old build's world, exactly
        Array<u8> bytes;
        CHECK(read_file(path, &bytes));
        remove(path);
        TextBuf v2;
        as_v2(file_text(bytes), kNewFeaturesOff, &v2);
        CHECK(strstr(v2.c_str(), "spark.activity") == nullptr && strstr(v2.c_str(), "world.archive") == nullptr);
        ReplayLog log;
        CHECK(log.parse(v2.view(), &err) && log.version() == 2);
        ReplayResult r;
        CHECK(replay_verify(log, 1, &r));
        CHECK(r.ok && r.checkpoints_checked == 6 && log.end().hash == 0x57978c34d6bf41e8ull);
    }
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

} // namespace

void suite_replay() {
    test_log_and_verify();
    test_detects_tampering();
    test_parse_errors();
    test_knob_scheduling_rules();
    test_legacy_config();
    test_legacy_logs();
}
