// nova -- command line front end for the Phase 0 engine.
//
//   nova run <config> [--threads N] [--ticks N]
//       Runs the config. Reads run.ticks, run.threads, stats.window,
//       replay.checkpoint_every and output.csv / output.replay / output.snapshot
//       (empty = off). Prints the final world hash.
//
//   nova replay <log> [--threads N]
//       Re-runs a replay log and checks every logged hash.
//
// Any config key that nothing reads is an error (typo protection).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "nova/config.h"
#include "nova/io.h"
#include "nova/replay.h"
#include "nova/run.h"

using namespace nova;

namespace {

int usage() {
    fprintf(stderr, "usage:\n"
                    "  nova run <config> [--threads N] [--ticks N]\n"
                    "  nova replay <log> [--threads N]\n"
                    "  (threads 0 = all hardware threads)\n");
    return 2;
}

double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

// Creates the parent directory of `path` (one level) if it is missing.
void ensure_parent_dir(const char* path) {
    char dir[MAX_PATH];
    usize n = strlen(path);
    if (n >= sizeof dir)
        return;
    memcpy(dir, path, n + 1);
    for (usize i = n; i-- > 0;) {
        if (dir[i] == '/' || dir[i] == '\\') {
            dir[i] = '\0';
            if (i > 0)
                CreateDirectoryA(dir, nullptr); // fails harmlessly if it exists
            return;
        }
    }
}

struct Args {
    const char* mode = nullptr;
    const char* path = nullptr;
    bool has_threads = false;
    u64 threads = 0;
    bool has_ticks = false;
    u64 ticks = 0;
};

bool parse_args(int argc, char** argv, Args* a) {
    if (argc < 3)
        return false;
    a->mode = argv[1];
    a->path = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (i + 1 >= argc)
            return false;
        if (strcmp(argv[i], "--threads") == 0) {
            a->has_threads = parse_u64(argv[++i], &a->threads) && a->threads <= 1024;
            if (!a->has_threads)
                return false;
        } else if (strcmp(argv[i], "--ticks") == 0) {
            a->has_ticks = parse_u64(argv[++i], &a->ticks);
            if (!a->has_ticks)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

int cmd_run(const Args& a) {
    Run run;
    Config& cfg = run.config();
    Config::ParseError pe;
    if (!cfg.load_file(a.path, &pe)) {
        fprintf(stderr, "nova: %s: %s\n", a.path, pe.msg);
        return 1;
    }

    u64 ticks = cfg.get_u64("run.ticks", 1000);
    u64 threads = cfg.get_u64("run.threads", 0);
    u64 window = cfg.get_u64("stats.window", 100);
    u64 every = cfg.get_u64("replay.checkpoint_every", 100);
    TextBuf csv, replay, snap; // copies: config views die on knob changes
    csv.append(cfg.get_str("output.csv", ""));
    replay.append(cfg.get_str("output.replay", ""));
    snap.append(cfg.get_str("output.snapshot", ""));
    if (a.has_ticks)
        ticks = a.ticks;
    if (a.has_threads)
        threads = a.threads;
    if (window == 0 || window > 0xFFFFFFFFull)
        cfg.fail("stats.window", "must be between 1 and 4294967295");
    if (threads > 1024)
        cfg.fail("run.threads", "must be between 0 and 1024");

    TextBuf err;
    if (!run.init(static_cast<u32>(threads), &err)) {
        fprintf(stderr, "nova: %s\n", err.c_str());
        return 1;
    }
    Str unused;
    if (cfg.first_unused(&unused)) {
        fprintf(stderr, "nova: config key '%.*s' is not used by anything (typo?)\n",
                static_cast<int>(unused.n), unused.p);
        return 1;
    }

    if (csv.size()) {
        ensure_parent_dir(csv.c_str());
        if (!run.open_stats(csv.c_str(), static_cast<u32>(window), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
    }
    if (replay.size()) {
        ensure_parent_dir(replay.c_str());
        if (!run.open_replay(replay.c_str(), every, &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
    }

    const World& w = run.world();
    printf("nova: seed %llu, %u nodes, %u edges, %llu ticks on %u threads\n",
           static_cast<unsigned long long>(w.seed()), w.node_count(), w.edge_count(),
           static_cast<unsigned long long>(ticks), run.threads());
    double t0 = now_ms();
    run.run_until(ticks);
    double ms = now_ms() - t0;
    run.finish();

    if (snap.size()) {
        ByteWriter bytes;
        run.snapshot(bytes);
        ensure_parent_dir(snap.c_str());
        if (!write_file(snap.c_str(), bytes.data(), bytes.size())) {
            fprintf(stderr, "nova: cannot write snapshot '%s'\n", snap.c_str());
            return 1;
        }
    }
    printf("nova: tick %llu  hash %016llx  units %llu  (%.1f ms)\n",
           static_cast<unsigned long long>(w.tick()), static_cast<unsigned long long>(w.hash()),
           static_cast<unsigned long long>(w.total_units()), ms);
    return 0;
}

int cmd_replay(const Args& a) {
    ReplayLog log;
    TextBuf err;
    if (!log.load_file(a.path, &err)) {
        fprintf(stderr, "nova: %s\n", err.c_str());
        return 1;
    }
    u32 threads = a.has_threads ? static_cast<u32>(a.threads) : 0;
    ReplayResult r;
    double t0 = now_ms();
    bool ok = replay_verify(log, threads, &r);
    double ms = now_ms() - t0;
    if (!ok) {
        fprintf(stderr, "nova: replay FAILED: %s\n", r.msg);
        return 1;
    }
    printf("nova: replay OK: %llu checkpoints match, %llu knob events (%.1f ms)\n",
           static_cast<unsigned long long>(r.checkpoints_checked),
           static_cast<unsigned long long>(log.knob_count()), ms);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, &a))
        return usage();
    if (strcmp(a.mode, "run") == 0)
        return cmd_run(a);
    if (strcmp(a.mode, "replay") == 0)
        return cmd_replay(a);
    return usage();
}
