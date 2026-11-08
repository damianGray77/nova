// nova -- command line front end for the engine.
//
//   nova run <config> [--threads N] [--ticks N] [--set key=value]...
//            [--stop-file PATH] [--no-esc] [--profile]
//       Runs the config. --set overrides or adds config entries (applied before
//       anything reads the config, so they are logged with it). Reads:
//         run.ticks             stop after this many ticks (0 = no tick limit)
//         run.seconds           stop at the first tick boundary after this much
//                               wall-clock time (0 = no limit)
//         run.after_replicator  stop this many ticks after the first replicator
//                               is detected (-1 = keep going)
//         run.threads, stats.window, replay.checkpoint_every
//         output.csv / .nodes / .signals / .replay / .snapshot / .genebank /
//                               .lineages / .result / .profiles / .arrivals
//                               paths, empty = off
//                               (.nodes and .signals need .csv)
//         output.snapshot_every also write <output.snapshot>.<tick> every N
//                               ticks (0 = off; Phase 6)
//       Esc in the console stops the run gracefully at the next tick (unless
//       --no-esc), and so does the appearance of the --stop-file path (how the
//       harness stops its children). Either way every output is finished and
//       the replay log records the tick it stopped at, so replay stays exact.
//       Prints the final world hash.
//
//   nova replay <log> [--threads N]
//       Re-runs a replay log and checks every logged hash and event.
//
//   nova resume <snapshot> [--threads N] [--ticks N] [--set key=value]...
//       Continues a snapshot headlessly up to tick N, printing its hash every
//       replay.checkpoint_every ticks; --set changes its settings first. For
//       bisecting a divergence; nothing is written.
//
//   nova bench <seconds> [--threads N]
//       Interpreter throughput on three workloads (bench.cpp), single-threaded
//       and on N threads. Build in Release first: tools/bench.ps1.
//
//   nova view ...
//       The analysis UI (Phase 6, view_cmd.cpp): a run watched live, a
//       snapshot continued, or a finished run's files browsed; with no config
//       (or --setup <config>) the setup page, every setting before a run
//       starts (docs/config-plan.md C2-C5).
//
//   nova keys [--md <path>]
//       The settings registry (nova/keys.h): every setting with its kind,
//       default, range, group and help, as a table; --md writes the Markdown
//       reference (docs/keys.md).
//
//   nova analyse novelty | tree | scores | roles | groups <run> ...,  nova search <file>
//       Learning from the runs (docs/analysis-plan.md, analyse_cmd.cpp):
//       the novelty meter, family trees and a run's scores from its files,
//       behaviour roles and group behaviour from its profiles, and the
//       settings search.
//
// Any config key with no registry entry is an error (typo protection); one the
// registry knows that nothing read (its feature off) is accepted, so a setup
// page's run.cfg, which names every setting, runs. Every setting's default and
// range live in its registry entry (core/src/keys.inc). A run's settings are
// fixed from its start (Run::lock_knobs).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "nova/pool.h"
#include "nova/accel.h"
#include "nova/config.h"
#include "nova/hash.h"
#include "nova/io.h"
#include "nova/keys.h"
#include "nova/legacy.h"
#include "nova/replay.h"
#include "nova/run.h"

using namespace nova;

int cmd_bench(double seconds, u32 threads); // bench.cpp
int cmd_bench_devices(u32 threads);          // bench.cpp
int cmd_bench_costs();                       // bench_costs.cpp
int cmd_bench_warp(int argc, char** argv);   // bench_warp.cpp
// Learning from the runs (docs/analysis-plan.md): analyse_cmd.cpp.
int cmd_analyse(int argc, char** argv); // nova analyse novelty | tree | scores <run>
int cmd_search(int argc, char** argv);  // nova search <search file>

// nova devices: the GPUs and NPU this machine offers, and what each accepts
// (Phase 5, design.md 6.11).
int cmd_devices() {
    AccelDevice devs[16];
    const u32 n = accel_list(devs, 16);
    printf("nova: %u accelerator device%s\n", n, n == 1 ? "" : "s");
    for (u32 i = 0; i < n; ++i) {
        const AccelDevice& d = devs[i];
        printf("[%u] %s -- %s%s, %.1f GB dedicated\n", i, d.name, d.gpu ? "GPU" : "NPU / compute accelerator",
               d.hardware ? (d.integrated ? ", integrated" : "") : ", software", static_cast<double>(d.memory) / 1e9);
        if (!d.hardware)
            continue;
        TextBuf report;
        if (d.gpu)
            accel_probe_compute(i, &report);
        const u32 exact = accel_probe_dml(i, &report);
        printf("%s  exact matrix multiply forms: %s%s%s%s%s%s\n", report.c_str(), exact & kProbeInt8 ? "int8 " : "",
               exact & kProbeFloat32 ? "float32 " : "", exact & kProbeFloat16 ? "float16 " : "",
               exact & kProbeFloat16Small ? "float16-small " : "", exact & kProbeGraph ? "(graph) " : "",
               exact ? "" : "none");
        if (d.npu && d.hardware) {
            printf("  float16 multiply shapes [batch, m x 32] x [batch, 32 x n] (batch in dimension 1):\n");
            fflush(stdout);
            const u32 shapes[][3] = {{1, 128, 128}, {8, 128, 128}, {16, 128, 128}, {32, 128, 128}, {64, 128, 128}};
            for (const auto& s : shapes) {
                double ms = 0;
                const bool ok = accel_probe_shape(i, s[0], s[1], s[2], 1, &ms);
                printf("    [%u, %ux32] x [%u, 32x%u]: %s, %.0f ms\n", s[0], s[1], s[0], s[2],
                       ok ? "compiles" : "refused", ms);
                fflush(stdout);
            }
        }
        fflush(stdout);
    }
    return 0;
}

// nova land <config> [--set key=value]...: what a landscape's settings
// generate (Phase 7): its landmasses, paths per node and bridges.
int cmd_land(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: nova land <config> [--set key=value]...\n");
        return 2;
    }
    Config cfg;
    Config::ParseError pe;
    if (!cfg.load_file(argv[2], &pe)) {
        fprintf(stderr, "nova: %s: %s\n", argv[2], pe.msg);
        return 1;
    }
    for (int i = 3; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--set") != 0 || !apply_set(cfg, argv[i + 1])) {
            fprintf(stderr, "usage: nova land <config> [--set key=value]...\n");
            return 2;
        }
    }
    cfg.set("detect.enabled", "false");
    cfg.set("accel.mode", "instant");
    cfg.set("world.disk", "0"); // only the landscape is wanted: no disk files
    cfg.set("disk.path", "");
    const double t0 = now_ms();
    World* w = new World;
    if (!w->init(cfg)) {
        fprintf(stderr, "nova: %s\n", cfg.value_error());
        return 1;
    }
    const double ms = now_ms() - t0;
    if (!w->landscape()) {
        fprintf(stderr, "nova: not a landscape (world.graph = landscape)\n");
        return 1;
    }
    const Landscape& L = w->land();
    const u32 n = w->node_count();
    Array<u32> per;
    per.resize(L.masses);
    for (u32 i = 0; i < n; ++i)
        ++per[L.mass[i]];
    Array<u64> sizes;
    for (u32 m = 0; m < L.masses; ++m)
        if (per[m])
            sizes.push((static_cast<u64>(per[m]) << 32) | m);
    sort_u64(sizes.data(), sizes.size());
    u32 hist[14] = {};
    for (u32 i = 0; i < n; ++i) {
        u32 land = 0;
        for (u32 k = 0; k < w->degree(i); ++k)
            land += !w->edge_bridge(w->port(i, k));
        ++hist[land < 13 ? land : 13];
    }
    usize cells = 0;
    for (usize c = 0; c < L.land.size(); ++c)
        cells += L.land[c];
    printf("nova: landscape of map seed %llu%s, %.2f x 1, %ux%u raster, %.0f%% land, generated in %.0f ms\n",
           static_cast<unsigned long long>(w->map_seed()), w->map_seed() == w->seed() ? " (the seed's)" : "",
           L.width, L.raster_w, L.raster_h, 100.0 * cells / L.land.size(), ms);
    printf("  %u nodes on %u landmasses (of %u); the largest:", n, static_cast<unsigned>(sizes.size()), L.masses);
    for (usize k = 0; k < sizes.size() && k < 10; ++k)
        printf(" %u", static_cast<unsigned>(sizes[sizes.size() - 1 - k] >> 32));
    printf("\n  paths per node (land):");
    for (u32 d = 0; d < 14; ++d)
        if (hist[d])
            printf("  %u%s: %u", d, d == 13 ? "+" : "", hist[d]);
    printf("\n  %u hermits, %u hubs, %u long links; %u bridges (open %u of every %u ticks); min spacing %.4f\n",
           L.hermits, L.hubs, L.long_added, L.bridges, w->land_params().open_ticks, w->land_params().open_every,
           L.spacing);
    // A varied world (docs/world-plan.md W8): the substrates, distinct ones
    // (each node's eight levels together) and each type's share per level.
    if (!w->substrates()) {
        printf("  substrates: off (substrate.enabled = false)\n");
    } else {
        Array<u64> subs;
        for (u32 i = 0; i < n; ++i) {
            u64 key = 0;
            for (u32 t = 0; t < kSubstrateTypes; ++t)
                key = key << 8 | w->substrate_level(i, t);
            subs.push(key);
        }
        sort_u64(subs.data(), subs.size());
        u32 distinct = 0;
        for (usize i = 0; i < subs.size(); ++i)
            distinct += i == 0 || subs[i] != subs[i - 1];
        const double fl = w->substrate_params().floor_mult;
        printf("  substrates: %u distinct over %u nodes (of %u possible); each type's share of nodes at %gx, 0.5x, "
               "1x, 1.5x, 2x:\n",
               distinct, n, 390625u, fl);
        for (u32 t = 0; t < kSubstrateTypes; ++t) {
            u32 at[kSubstrateLevels] = {};
            for (u32 i = 0; i < n; ++i)
                ++at[w->substrate_level(i, t)];
            printf("    %-12s", substrate_label(t));
            for (u32 l = 0; l < kSubstrateLevels; ++l)
                printf(" %5.1f%%", n ? 100.0 * at[l] / n : 0.0);
            printf("\n");
        }
    }
    delete w;
    return 0;
}

// nova keys [--md <path>]: the settings registry (docs/config-plan.md C1, C5):
// every setting as a table, or the Markdown reference (docs/keys.md).
int cmd_keys(int argc, char** argv) {
    TextBuf out;
    if (argc == 2) {
        keys_table(out);
        fwrite(out.view().p, 1, out.size(), stdout);
        return 0;
    }
    if (argc == 4 && strcmp(argv[2], "--md") == 0) {
        keys_markdown(out);
        ensure_parent_dir(argv[3]);
        if (!write_file(argv[3], out.view().p, out.size())) {
            fprintf(stderr, "nova: cannot write '%s'\n", argv[3]);
            return 1;
        }
        printf("nova: wrote %s (%llu settings)\n", argv[3], static_cast<unsigned long long>(key_count()));
        return 0;
    }
    fprintf(stderr, "usage: nova keys [--md <path>]\n");
    return 2;
}

namespace {

int usage() {
    fprintf(stderr, "usage:\n"
                    "  nova run <config> [--threads N] [--ticks N] [--set key=value]...\n"
                    "           [--stop-file PATH] [--no-esc] [--profile] [--profile-csv PATH]\n"
                    "  nova replay <log> [--threads N]\n"
                    "  nova resume <snapshot> [--threads N] [--ticks N] [--set key=value]...\n"
                    "              (continued headlessly to tick N, hashes printed; nothing written)\n"
                    "  nova bench <seconds per measurement> [--threads N]\n"
                    "  nova bench devices [--threads N]   (the GPU and NPU against the CPU reference)\n"
                    "  nova bench costs                   (this machine's basic operations, for cost tables)\n"
                    "  nova costs [classic | home_v1]     (a cost table: each operation and instruction)\n"
                    "  nova devices                       (what each accelerator accepts)\n"
                    "  nova view                          (the setup page on the default preset:\n"
                    "                                      every setting, then Start)\n"
                    "  nova view --setup <config> [--set key=value]...  (the setup page on that file)\n"
                    "  nova view <config | snapshot> [--threads N] [--set key=value]... [--no-output]\n"
                    "            [--render out.png --ticks N [--size WxH]]\n"
                    "  nova view --run <run directory> [--render out.png [--window K] [--size WxH]]\n"
                    "  nova land <config> [--set key=value]...  (what a landscape's settings generate)\n"
                    "  nova keys [--md <path>]            (every setting: kind, default, range, help;\n"
                    "                                      --md writes the reference, docs/keys.md)\n"
                    // Learning from the runs (docs/analysis-plan.md): analyse_cmd.cpp.
                    "  nova analyse novelty | roles | groups | tree | scores <run directory> [options]\n"
                    "                                     (reads a run's files; nova analyse alone for the options)\n"
                    "  nova search <search file> [--dry-run]  (the settings search: trials, scores, a model)\n"
                    "  (threads 0 = all hardware threads)\n");
    return 2;
}

// Esc in the console window, read from the console input queue without
// blocking. Does nothing when stdin is not a console.
struct EscWatch {
    HANDLE in = nullptr;
    bool console = false;

    void start() {
        in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        console = in && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode);
    }
    bool pressed() {
        if (!console)
            return false;
        DWORD n = 0;
        while (GetNumberOfConsoleInputEvents(in, &n) && n > 0) {
            INPUT_RECORD rec;
            DWORD got = 0;
            if (!ReadConsoleInputA(in, &rec, 1, &got) || got == 0)
                return false;
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown &&
                rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
                return true;
        }
        return false;
    }
};

bool file_exists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

struct Args {
    const char* mode = nullptr;
    const char* path = nullptr;
    bool has_threads = false;
    u64 threads = 0;
    bool has_ticks = false;
    u64 ticks = 0;
    const char* sets[256] = {}; // --set key=value, in order
    int set_count = 0;
    const char* stop_file = nullptr;
    bool no_esc = false;
    bool profile = false; // --profile: where each tick's time goes (Phase 7)
    const char* profile_csv = nullptr; // --profile-csv PATH: the same every 1,000 ticks, as CSV
    // Bisecting a divergence (run and resume): the world's hash every tick
    // from tick N on, and each node's hash at the end, a line each, to a file.
    bool has_hashes_from = false;
    u64 hashes_from = 0;
    const char* node_hashes = nullptr;
    const char* dump_node = nullptr; // --dump-node N:PREFIX: node N's state at the end, a section a file
    // --dump-round T:PATH: the first GPU warp round of tick T (or later), for `nova bench warp`
    const char* dump_round = nullptr;
};

bool parse_args(int argc, char** argv, Args* a) {
    if (argc < 3)
        return false;
    a->mode = argv[1];
    a->path = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--no-esc") == 0) {
            a->no_esc = true;
            continue;
        }
        if (strcmp(argv[i], "--profile") == 0) {
            a->profile = true;
            continue;
        }
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
        } else if (strcmp(argv[i], "--profile-csv") == 0) {
            a->profile_csv = argv[++i];
            a->profile = true;
        } else if (strcmp(argv[i], "--hashes-from") == 0) {
            a->has_hashes_from = parse_u64(argv[++i], &a->hashes_from);
            if (!a->has_hashes_from)
                return false;
        } else if (strcmp(argv[i], "--node-hashes") == 0) {
            a->node_hashes = argv[++i];
        } else if (strcmp(argv[i], "--dump-node") == 0) {
            a->dump_node = argv[++i];
        } else if (strcmp(argv[i], "--dump-round") == 0) {
            a->dump_round = argv[++i];
        } else if (strcmp(argv[i], "--set") == 0) {
            if (a->set_count == static_cast<int>(sizeof a->sets / sizeof a->sets[0]))
                return false;
            a->sets[a->set_count++] = argv[++i];
        } else if (strcmp(argv[i], "--stop-file") == 0) {
            a->stop_file = argv[++i];
        } else {
            return false;
        }
    }
    return true;
}

// Applies each --set key=value to the config. False (with a message) on a
// malformed entry.
bool apply_sets(const Args& a, Config& cfg) {
    for (int i = 0; i < a.set_count; ++i)
        if (!apply_set(cfg, a.sets[i]))
            return false;
    return true;
}

// --hashes-from: the world's hash after this tick, if it's in the window.
void print_tick_hash(const Args& a, const World& w) {
    if (a.has_hashes_from && w.tick() >= a.hashes_from) {
        printf("tickhash %llu %016llx\n", static_cast<unsigned long long>(w.tick()),
               static_cast<unsigned long long>(w.hash()));
        fflush(stdout);
    }
}

// --node-hashes: each node's own hash, a line each ("node tick hash").
void write_node_hashes(const Args& a, const World& w) {
    if (!a.node_hashes || w.model() != Model::soup)
        return;
    FILE* f = fopen(a.node_hashes, "w");
    if (!f) {
        fprintf(stderr, "nova: cannot write %s\n", a.node_hashes);
        return;
    }
    w.hash(); // (brings every lane home first)
    for (u32 n = 0; n < w.node_count(); ++n) {
        Hasher h(0x6E6F64652D686173ull);
        w.node_soup(n).hash_into(h);
        fprintf(f, "%u %llu %016llx\n", n, static_cast<unsigned long long>(w.tick()),
                static_cast<unsigned long long>(h.finish()));
    }
    fclose(f);
}

// --dump-node N:PREFIX: node N's state, a section a file (Soup::debug_dump).
void dump_node(const Args& a, const World& w) {
    if (!a.dump_node || w.model() != Model::soup)
        return;
    const char* colon = strchr(a.dump_node, ':');
    u64 n = 0;
    if (!colon || !parse_u64(Str(a.dump_node, static_cast<usize>(colon - a.dump_node)), &n) || n >= w.node_count()) {
        fprintf(stderr, "nova: --dump-node expects N:PREFIX\n");
        return;
    }
    w.hash(); // (brings every lane home first)
    w.node_soup(static_cast<u32>(n)).debug_dump(colon + 1);
}

// Prints the accelerator notes from byte `from` on, a line each with the
// nova prefix; returns how far it got.
usize print_notes(Str notes, usize from) {
    usize start = from;
    for (usize i = from; i < notes.n; ++i) {
        if (notes.p[i] != '\n')
            continue;
        printf("nova: %.*s\n", static_cast<int>(i - start), notes.p + start);
        start = i + 1;
    }
    fflush(stdout);
    return start;
}

} // namespace

namespace nova {

double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

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

bool apply_set(Config& cfg, const char* kv) {
    Str s = kv;
    usize eq = s.find('=');
    Str key = eq < s.n ? s.sub(0, eq).trim() : Str();
    Str value = eq < s.n ? s.sub(eq + 1, s.n - eq - 1).trim() : Str();
    if (eq == s.n || !Config::valid_key(key) || value.find('#') < value.n || value.find('\n') < value.n) {
        fprintf(stderr, "nova: --set '%s': expected key=value\n", kv);
        return false;
    }
    cfg.set(key, value);
    return true;
}

void read_run_keys(Config& cfg, RunKeys* k) {
    // Defaults and ranges: the settings registry (core/src/keys.inc).
    k->ticks = key_u64(cfg, "run.ticks");
    k->threads = key_u64(cfg, "run.threads");
    k->seconds = key_f64(cfg, "run.seconds");
    k->after = key_i64(cfg, "run.after_replicator");
    k->window = key_u64(cfg, "stats.window");
    k->every = key_u64(cfg, "replay.checkpoint_every");
    k->snapshot_every = key_u64(cfg, "output.snapshot_every");
    k->nodes_every = key_u64(cfg, "stats.nodes_every");
    k->budget_threads.append(key_str(cfg, "budget.threads"));
    k->priority.append(key_choice(cfg, "budget.priority"));
    {
        const Str t = k->budget_threads.view();
        u64 v = 0;
        double pc = 0;
        const bool ok = t.eq("all") || t.eq("all_but_one") || (parse_u64(t, &v) && v >= 1 && v <= 1024) ||
                        (t.n > 1 && t.p[t.n - 1] == '%' && parse_f64(t.sub(0, t.n - 1), &pc) && pc > 0 && pc <= 100);
        if (!ok)
            cfg.fail("budget.threads", "must be all, all_but_one, a count or a percentage");
    }
    k->csv.append(key_str(cfg, "output.csv"));
    k->nodes.append(key_str(cfg, "output.nodes"));
    k->signals.append(key_str(cfg, "output.signals"));
    k->replay.append(key_str(cfg, "output.replay"));
    k->snap.append(key_str(cfg, "output.snapshot"));
    k->bank.append(key_str(cfg, "output.genebank"));
    k->lin.append(key_str(cfg, "output.lineages"));
    k->result.append(key_str(cfg, "output.result"));
    k->prof.append(key_str(cfg, "output.profiles")); // behaviour profiles (A3)
    k->arr.append(key_str(cfg, "output.arrivals"));
    if (k->snapshot_every && k->snap.size() == 0)
        cfg.fail("output.snapshot_every", "needs output.snapshot (the numbered snapshots' path)");
}

u32 machine_threads(const RunKeys& k, u64 explicit_threads) {
    if (explicit_threads)
        return static_cast<u32>(explicit_threads);
    const u32 hw = ThreadPool::hardware_threads();
    const Str t = k.budget_threads.view();
    if (t.eq("all"))
        return hw;
    if (t.eq("all_but_one") || t.n == 0)
        return hw > 1 ? hw - 1 : 1;
    u64 v = 0;
    if (parse_u64(t, &v))
        return static_cast<u32>(v);
    double pc = 0;
    parse_f64(t.sub(0, t.n - 1), &pc);
    const u32 n = static_cast<u32>(hw * pc / 100.0 + 0.5);
    return n ? n : 1;
}

void apply_priority(const RunKeys& k) {
    const Str p = k.priority.view();
    const DWORD cls = p.eq("normal") ? NORMAL_PRIORITY_CLASS
                                     : (p.eq("idle") ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    SetPriorityClass(GetCurrentProcess(), cls);
}

void print_budget_notes(const Run& run) {
    const Str n = run.budget_notes();
    usize start = 0;
    for (usize i = 0; i < n.n; ++i) {
        if (n.p[i] != '\n')
            continue;
        printf("nova: %.*s\n", static_cast<int>(i - start), n.p + start);
        start = i + 1;
    }
}

bool open_outputs(Run& run, RunKeys& k) {
    TextBuf err;
    RunKeys& m = k;
    if (k.csv.size()) {
        ensure_parent_dir(m.csv.c_str());
        if (!run.open_stats(m.csv.c_str(), static_cast<u32>(k.window), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.nodes.size()) {
        ensure_parent_dir(m.nodes.c_str());
        if (!run.open_nodes(m.nodes.c_str(), &err, static_cast<u32>(k.nodes_every))) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.signals.size()) {
        ensure_parent_dir(m.signals.c_str());
        if (!run.open_signals(m.signals.c_str(), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.replay.size()) {
        ensure_parent_dir(m.replay.c_str());
        if (!run.open_replay(m.replay.c_str(), k.every, &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.bank.size()) {
        ensure_parent_dir(m.bank.c_str());
        if (!run.open_genebank(m.bank.c_str(), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.lin.size()) {
        ensure_parent_dir(m.lin.c_str());
        if (!run.open_lineages(m.lin.c_str(), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    // Behaviour profiles (docs/analysis-plan.md A3): profiles.csv and arrivals.csv.
    if (k.prof.size()) {
        ensure_parent_dir(m.prof.c_str());
        if (!run.open_profiles(m.prof.c_str(), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.arr.size()) {
        ensure_parent_dir(m.arr.c_str());
        if (!run.open_arrivals(m.arr.c_str(), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return false;
        }
    }
    if (k.snapshot_every) {
        ensure_parent_dir(m.snap.c_str());
        run.open_snapshots(m.snap.c_str(), k.snapshot_every);
    }
    return true;
}

bool finish_outputs(const Run& run, RunKeys& k, const char* stop, double seconds) {
    RunKeys& m = k;
    if (k.result.size()) {
        ensure_parent_dir(m.result.c_str());
        if (!write_result(m.result.c_str(), run, stop, seconds)) {
            fprintf(stderr, "nova: cannot write result '%s'\n", m.result.c_str());
            return false;
        }
    }
    if (k.snap.size()) {
        ByteWriter bytes;
        run.snapshot(bytes);
        ensure_parent_dir(m.snap.c_str());
        if (!write_file(m.snap.c_str(), bytes.data(), bytes.size())) {
            fprintf(stderr, "nova: cannot write snapshot '%s'\n", m.snap.c_str());
            return false;
        }
    }
    return true;
}

// result.txt: what a harness needs to know about a finished run, as
// `key = value` lines (the config format, so it parses back).
bool write_result(const char* path, const Run& run, const char* stop, double seconds) {
    const World& w = run.world();
    TextBuf t;
    auto kv = [&t](const char* k) {
        t.append(k);
        t.append(" = ");
    };
    kv("seed");
    t.append_u64(w.seed());
    t.append_char('\n');
    kv("map_seed"); // world.seed's number (the seed when it isn't set)
    t.append_u64(w.map_seed());
    t.append_char('\n');
    kv("end_tick");
    t.append_u64(w.tick());
    t.append_char('\n');
    kv("stop");
    t.append(stop);
    t.append_char('\n');
    kv("seconds");
    t.append_f64(static_cast<double>(static_cast<i64>(seconds * 10 + 0.5)) / 10);
    t.append_char('\n');
    kv("instructions");
    t.append_u64(run.instructions());
    t.append_char('\n');
    kv("hash");
    t.append_hex64(w.hash());
    t.append_char('\n');
    if (w.model() == Model::soup) {
        kv("table");
        t.append(w.soup_params().table.name);
        t.append_char('\n');
        kv("selectors");
        describe_selectors(w.soup_params().sel, t);
        t.append_char('\n');
        u64 links = 0, inject = 0, founded = 0;
        u32 depth = 0;
        for (u32 n = 0; n < w.node_count(); ++n) {
            if (const Lineages* L = w.node_lineages(n)) {
                links += L->links();
                inject += L->inject_links();
                founded += L->founded();
                depth = max_of(depth, L->max_depth_ever());
            }
        }
        kv("links");
        t.append_u64(links);
        t.append_char('\n');
        kv("inject_links");
        t.append_u64(inject);
        t.append_char('\n');
        u64 gpu_links = 0;
        for (u32 n = 0; n < w.node_count(); ++n)
            if (const Lineages* L = w.node_lineages(n))
                gpu_links += L->gpu_links();
        kv("gpus");
        t.append_u64(w.soup_params().gpus);
        t.append_char('\n');
        kv("gpu_links");
        t.append_u64(gpu_links);
        t.append_char('\n');
        const Detection& g = run.gpu_detection();
        kv("gpu_replicator");
        t.append(g.found ? "true" : "false");
        t.append_char('\n');
        if (g.found) {
            kv("gpu_detect_tick");
            t.append_u64(g.tick);
            t.append_char('\n');
            kv("gpu_detect_instructions");
            t.append_u64(g.instructions);
            t.append_char('\n');
            kv("gpu_detect_lineage");
            append_lineage(t, g.lineage);
            t.append_char('\n');
        }
        kv("npus");
        t.append_u64(w.soup_params().npus);
        t.append_char('\n');
        const Detection& np = run.npu_detection();
        kv("npu_user");
        t.append(np.found ? "true" : "false");
        t.append_char('\n');
        if (np.found) {
            kv("npu_detect_tick");
            t.append_u64(np.tick);
            t.append_char('\n');
            kv("npu_detect_lineage");
            append_lineage(t, np.lineage);
            t.append_char('\n');
        }
        // Phase 3: the world.
        kv("graph");
        t.append(w.procedural() ? "procedural" : "ring");
        t.append_char('\n');
        kv("dispersal");
        t.append(w.dispersal() ? "true" : "false");
        t.append_char('\n');
        u64 colonisations = 0;
        for (u32 n = 0; n < w.node_count(); ++n)
            if (const Lineages* L = w.node_lineages(n))
                colonisations += L->colonisations();
        kv("colonisations");
        t.append_u64(colonisations);
        t.append_char('\n');
        const Detection& col = run.colonist();
        kv("colonist");
        t.append(col.found ? "true" : "false");
        t.append_char('\n');
        if (col.found) {
            kv("colonist_tick");
            t.append_u64(col.tick);
            t.append_char('\n');
            kv("colonist_instructions");
            t.append_u64(col.instructions);
            t.append_char('\n');
            kv("colonist_node");
            t.append_u64(col.node);
            t.append_char('\n');
            kv("colonist_lineage");
            append_lineage(t, col.lineage);
            t.append_char('\n');
            kv("colonist_len");
            t.append_u64(col.len);
            t.append_char('\n');
        }
        // Phase 5: the accelerators: where async ops ran, and how many.
        kv("accel_mode");
        t.append(w.accel_async() ? "async" : "instant");
        t.append_char('\n');
        if (w.accel_async()) {
            kv("accel_latency");
            t.append_u64(w.soup_params().accel_latency);
            t.append_char('\n');
            kv("accel_gpu");
            t.append(w.accel_gpu_name());
            t.append_char('\n');
            kv("accel_npu");
            t.append(w.accel_npu_name());
            t.append_char('\n');
            const char* const keys[] = {"accel_device_ops", "accel_npu_ops", "accel_reference_ops", "accel_verified",
                                        "accel_mismatches"};
            const u64 vals[] = {w.accel_device_ops(), w.accel_npu_ops(), w.accel_reference_ops(), w.accel_verified(),
                                w.accel_mismatches()};
            for (int i = 0; i < 5; ++i) {
                kv(keys[i]);
                t.append_u64(vals[i]);
                t.append_char('\n');
            }
        }
        // Phase 9: the cost table (classic: as before, no key).
        if (w.model() == Model::soup && w.soup_params().cost.timed) {
            kv("cost_table");
            t.append(w.soup_params().cost.name);
            t.append_char('\n');
        }
        // Phase 8: warps: where they ran, and how many rounds where.
        if (w.warps()) {
            kv("warp_device");
            t.append(w.warp_device_name());
            t.append_char('\n');
            const char* const wkeys[] = {"warp_gpu_rounds",  "warp_reference_rounds", "warp_gpu_body_rounds",
                                         "warp_reference_body_rounds", "warp_ticks_without_gpu",
                                         "warp_verified",    "warp_mismatches"};
            const u64 wvals[] = {w.warp_gpu_rounds(), w.warp_ref_rounds(),    w.warp_gpu_bodies(), w.warp_ref_bodies(),
                                 w.warp_plain_ticks(), w.warp_verified(), w.warp_mismatches()};
            for (int i = 0; i < 7; ++i) {
                kv(wkeys[i]);
                t.append_u64(wvals[i]);
                t.append_char('\n');
            }
        }
        // Phase 4: the lifecycle's totals, then the milestones.
        kv("collapses");
        t.append_u64(w.collapses_total());
        t.append_char('\n');
        kv("recoveries");
        t.append_u64(w.recoveries_total());
        t.append_char('\n');
        // Phase 8: with warps, the first replication on the GPU alone too.
        // Phase 9: with a timed cost table, the first lineage to defend itself.
        // docs/lifehistory-plan.md V7: with voluntary sleep, the first gated lineage to sleep.
        const Detection* const milestones[] = {&run.disk_user(), &run.signaller(), &run.gpu_native(),
                                               &run.defended(), &run.sleeper()};
        const char* const names[] = {"disk_user", "signaller", "gpu_native", "defended", "sleeper"};
        const bool on[] = {true, true, w.soup_params().warps, w.soup_params().cost.timed != 0, w.soup_params().sleep};
        for (int i = 0; i < 5; ++i) {
            if (!on[i])
                continue;
            const Detection& m = *milestones[i];
            kv(names[i]);
            t.append(m.found ? "true" : "false");
            t.append_char('\n');
            if (!m.found)
                continue;
            t.append(names[i]);
            t.append("_tick = ");
            t.append_u64(m.tick);
            t.append_char('\n');
            t.append(names[i]);
            t.append("_node = ");
            t.append_u64(m.node);
            t.append_char('\n');
            t.append(names[i]);
            t.append("_lineage = ");
            append_lineage(t, m.lineage);
            t.append_char('\n');
        }
        kv("lineages_founded");
        t.append_u64(founded);
        t.append_char('\n');
        kv("max_depth");
        t.append_u64(depth);
        t.append_char('\n');
        const Detection& d = run.detection();
        kv("detected");
        t.append(d.found ? "true" : "false");
        t.append_char('\n');
        if (d.found) {
            kv("detect_tick");
            t.append_u64(d.tick);
            t.append_char('\n');
            kv("detect_instructions");
            t.append_u64(d.instructions);
            t.append_char('\n');
            kv("detect_node");
            t.append_u64(d.node);
            t.append_char('\n');
            kv("detect_lineage");
            append_lineage(t, d.lineage);
            t.append_char('\n');
            kv("detect_depth");
            t.append_u64(d.depth);
            t.append_char('\n');
            kv("detect_alive");
            t.append_u64(d.alive);
            t.append_char('\n');
            kv("detect_len");
            t.append_u64(d.len);
            t.append_char('\n');
            kv("detect_genotype");
            t.append_hex64(d.genotype);
            t.append_char('\n');
            kv("detect_via");
            t.append(d.via == kViaInject ? "inject" : "divide");
            t.append_char('\n');
            kv("detect_genome");
            t.append(run.detection_genome());
            t.append_char('\n');
        }
    }
    return write_file(path, t.view().p, t.size());
}

} // namespace nova

namespace {

int cmd_run(const Args& a) {
    Run run;
    Config& cfg = run.config();
    Config::ParseError pe;
    if (!cfg.load_file(a.path, &pe)) {
        fprintf(stderr, "nova: %s: %s\n", a.path, pe.msg);
        return 1;
    }
    if (!apply_sets(a, cfg))
        return 1;

    RunKeys k;
    read_run_keys(cfg, &k);
    if (a.has_ticks)
        k.ticks = a.ticks;
    if (a.has_threads)
        k.threads = a.threads;
    const u64 ticks = k.ticks;
    const double seconds = k.seconds;
    const i64 after = k.after;

    TextBuf err;
    apply_priority(k);
    if (!run.init(machine_threads(k, k.threads), &err)) {
        fprintf(stderr, "nova: %s\n", err.c_str());
        return 1;
    }
    print_budget_notes(run);
    run.lock_knobs(); // a run's settings are fixed from its start (docs/config-plan.md C3)
    // A key with no registry entry is a typo; one the registry knows that
    // nothing read (its feature off, as in a setup page's run.cfg) is not.
    key_accept_known(cfg);
    Str unused;
    if (cfg.first_unused(&unused)) {
        fprintf(stderr, "nova: config key '%.*s' is not used by anything (typo?)\n",
                static_cast<int>(unused.n), unused.p);
        return 1;
    }
    if (!open_outputs(run, k))
        return 1;
    StepProfile prof;
    if (a.profile)
        run.world().set_profile(&prof);

    const World& w = run.world();
    printf("nova: seed %llu, ", static_cast<unsigned long long>(w.seed()));
    if (w.map_seed() != w.seed())
        printf("map seed %llu, ", static_cast<unsigned long long>(w.map_seed()));
    printf("%u nodes, %u edges, ", w.node_count(), w.edge_count());
    if (ticks)
        printf("%llu ticks", static_cast<unsigned long long>(ticks));
    else
        printf("no tick limit");
    if (seconds > 0)
        printf(", at most %.0f s", seconds);
    printf(" on %u threads\n", run.threads());
    if (w.model() == Model::soup) {
        TextBuf sel;
        describe_selectors(w.soup_params().sel, sel);
        printf("nova: table %s, selectors %s\n", w.soup_params().table.name, sel.c_str());
    }
    usize notes_shown = 0;
    if (w.accel_async()) {
        printf("nova: accelerators async, latency %u: GPU ops on %s, NPU ops on %s\n", w.soup_params().accel_latency,
               w.accel_gpu_name(), w.accel_npu_name());
        notes_shown = print_notes(w.accel_notes(), 0);
    }
    usize warp_notes_shown = 0;
    if (w.warps()) { // Phase 8
        printf("nova: warps of %u lanes on %s", w.soup_params().warp_lanes, w.warp_device_name());
        if (w.warp_compile_ms() > 100)
            printf(" (kernel compiled in %.1f s)", w.warp_compile_ms() / 1000.0);
        printf("\n");
        warp_notes_shown = print_notes(w.warp_notes(), 0);
    }
    fflush(stdout);

    // --profile-csv: every 1,000 ticks, where the time went (ms a tick, the
    // nodes' parts in thread-ms a tick), what warp.device = auto chose, and
    // how much the world did.
    FILE* pcsv = nullptr;
    StepProfile pmark;
    u64 pmark_plain = w.warp_plain_ticks(), pmark_split = w.warp_split_ticks(), pmark_gn = w.warp_gpu_bodies(),
        pmark_rn = w.warp_ref_bodies();
    double pmark_wall = now_ms();
    u64 pw_steps = 0, pw_lanes = 0, pw_instr = 0;
    if (a.profile_csv) {
        ensure_parent_dir(a.profile_csv);
        pcsv = fopen(a.profile_csv, "w");
        if (!pcsv) {
            fprintf(stderr, "nova: cannot write %s\n", a.profile_csv);
            return 1;
        }
        fprintf(pcsv, "tick,ticks,wall_ms,update,warp_cpu,warp_gpu,accel,barrier,after,total,"
                      "n_first,n_bodies,n_pack,n_ref,n_unpack,n_host,n_second,"
                      "plain_ticks,gpu_ticks,gpu_body_share,warp_limit,bodies,warps,warp_steps,lane_instr,instructions\n");
    }
    EscWatch esc;
    if (!a.no_esc)
        esc.start();
    const char* stop = "ticks";
    double t0 = now_ms(), last_poll = t0;
    usize announced = 0;
    while (ticks == 0 || w.tick() < ticks) {
        run.step();
        print_tick_hash(a, w);
        if (pcsv) {
            const SoupTally& lt = w.last_soup_tick();
            pw_steps += lt.warp_steps;
            pw_lanes += lt.lane_instr;
            pw_instr += lt.instructions;
            if (prof.ticks - pmark.ticks >= 1000) {
                const double n = static_cast<double>(prof.ticks - pmark.ticks), per = 1000.0 / n;
                const double barrier = (prof.signals + prof.departures + prof.delivery + prof.links + prof.lifecycle +
                                        prof.tally + prof.ports) -
                                       (pmark.signals + pmark.departures + pmark.delivery + pmark.links +
                                        pmark.lifecycle + pmark.tally + pmark.ports);
                const double parts[] = {prof.update - pmark.update, prof.warp_cpu - pmark.warp_cpu,
                                        prof.warp_gpu - pmark.warp_gpu, prof.accel - pmark.accel, barrier,
                                        prof.after - pmark.after};
                double total = 0;
                for (double p : parts)
                    total += p;
                const u64 gn = w.warp_gpu_bodies() - pmark_gn, rn = w.warp_ref_bodies() - pmark_rn;
                u64 bodies = 0;
                for (u32 i = 0; i < w.node_count(); ++i)
                    bodies += w.node_soup(i).body_count();
                u64 warps = 0;
                for (u32 i = 0; i < w.node_count(); ++i)
                    warps += w.node_soup(i).warps_on() ? w.node_soup(i).warps_alive() : 0;
                const double wall = now_ms();
                fprintf(pcsv,
                        "%llu,%.0f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,"
                        "%.3f,%u,%llu,%llu,%.0f,%.0f,%.0f\n",
                        static_cast<unsigned long long>(w.tick()), n, (wall - pmark_wall) / n, parts[0] * per,
                        parts[1] * per, parts[2] * per, parts[3] * per, parts[4] * per, parts[5] * per, total * per,
                        (prof.n_first - pmark.n_first) * per, (prof.n_bodies - pmark.n_bodies) * per,
                        (prof.n_pack - pmark.n_pack) * per, (prof.n_ref - pmark.n_ref) * per,
                        (prof.n_unpack - pmark.n_unpack) * per, (prof.n_host - pmark.n_host) * per,
                        (prof.n_second - pmark.n_second) * per,
                        static_cast<unsigned long long>(w.warp_plain_ticks() - pmark_plain),
                        static_cast<unsigned long long>(w.warp_split_ticks() - pmark_split),
                        gn + rn ? static_cast<double>(gn) / static_cast<double>(gn + rn) : 0.0, w.warp_limit(),
                        static_cast<unsigned long long>(bodies), static_cast<unsigned long long>(warps),
                        pw_steps / n, pw_lanes / n, pw_instr / n);
                fflush(pcsv);
                pmark = prof;
                pmark_plain = w.warp_plain_ticks();
                pmark_split = w.warp_split_ticks();
                pmark_gn = w.warp_gpu_bodies();
                pmark_rn = w.warp_ref_bodies();
                pmark_wall = wall;
                pw_steps = pw_lanes = pw_instr = 0;
            }
        }
        for (; announced < run.event_count(); ++announced) {
            printf("nova: tick %llu: %.*s\n", static_cast<unsigned long long>(run.event_tick(announced)),
                   static_cast<int>(run.event_text(announced).n), run.event_text(announced).p);
            fflush(stdout);
        }
        run.drop_events(); // printed (and in the replay log): no need to hold them for hours
        if (w.accel_notes().n > notes_shown) // a device failed, or verification found a difference
            notes_shown = print_notes(w.accel_notes(), notes_shown);
        if (w.accel_mismatches()) {
            stop = "accel_verify";
            break;
        }
        if (w.warp_notes().n > warp_notes_shown) // the GPU failed, or warp.verify found a difference
            warp_notes_shown = print_notes(w.warp_notes(), warp_notes_shown);
        if (w.warp_mismatches()) {
            stop = "warp_verify";
            break;
        }
        if (w.warp_failed()) {
            stop = "warp_device_error";
            break;
        }
        if (run.snapshot_error().n) {
            fprintf(stderr, "nova: %.*s\n", static_cast<int>(run.snapshot_error().n), run.snapshot_error().p);
            stop = "snapshot_error";
            break;
        }
        const Detection& d = run.detection();
        if (after >= 0 && d.found && w.tick() >= d.tick + 1 + static_cast<u64>(after)) {
            stop = "replicator";
            break;
        }
        double now = now_ms();
        if (seconds > 0 && now - t0 >= seconds * 1000.0) {
            stop = "seconds";
            break;
        }
        if (now - last_poll >= 200.0) { // keyboard and stop file: a few times a second
            last_poll = now;
            if (esc.pressed()) {
                stop = "esc";
                break;
            }
            if (a.stop_file && file_exists(a.stop_file)) {
                stop = "stop_file";
                break;
            }
        }
    }
    double ms = now_ms() - t0;
    if (pcsv)
        fclose(pcsv);
    write_node_hashes(a, w);
    dump_node(a, w);
    run.finish();
    if (strcmp(stop, "ticks") != 0)
        printf("nova: stopped (%s) at tick %llu\n", stop, static_cast<unsigned long long>(w.tick()));
    if (a.profile && prof.ticks) {
        const char* const names[] = {"update (parallel)", "warps: CPU passes", "warps: run", "accelerators",
                                     "signals",           "departures",        "delivery",         "link failures",
                                     "lifecycle",         "tallies",           "ports",            "after the tick"};
        const double parts[] = {prof.update,  prof.warp_cpu, prof.warp_gpu, prof.accel,     prof.signals, prof.departures,
                                prof.delivery, prof.links,   prof.lifecycle, prof.tally,    prof.ports,   prof.after};
        double total = 0;
        for (double p : parts)
            total += p;
        printf("nova: profile over %llu ticks, ms per tick:\n", static_cast<unsigned long long>(prof.ticks));
        for (int i = 0; i < 12; ++i)
            if (parts[i] > 0)
                printf("  %-18s %9.4f  %5.1f%%\n", names[i], parts[i] * 1000.0 / prof.ticks,
                       total > 0 ? 100.0 * parts[i] / total : 0.0);
        printf("  %-18s %9.4f\n", "total", total * 1000.0 / prof.ticks);
        if (w.model() == Model::soup && w.soup_params().cost.timed) { // Phase 9: world time against real time
            const double tick_us = w.soup_params().tick_units / 1e6; // picoseconds -> microseconds
            const double wall_us = total * 1e6 / prof.ticks;
            printf("  world time (%s): %.2f us a tick, so the world runs %.0f times slower than real time\n",
                   w.soup_params().cost.name, tick_us, tick_us > 0 ? wall_us / tick_us : 0.0);
        }
        if (prof.warp_cpu > 0)
            printf("  (warps' CPU passes: %.4f packing, %.4f unpacking and host instructions)\n",
                   prof.warp_pack * 1000.0 / prof.ticks, prof.warp_host * 1000.0 / prof.ticks);
        const u64 gn = w.warp_gpu_bodies(), rn = w.warp_ref_bodies();
        if (w.warps() && w.warp_auto() && w.warp_plain_ticks() > 0) // Phase 8: auto's choice, tick by tick
            printf("  warp.device auto: %llu ticks with the GPU, %llu without it\n",
                   static_cast<unsigned long long>(w.warp_split_ticks()),
                   static_cast<unsigned long long>(w.warp_plain_ticks()));
        if (w.warps() && gn + rn > 0) // Phase 8: how the rounds were shared
            printf("  warp rounds%s: %.0f%% of the body-rounds on the GPU, %.0f%% on the CPU at once\n",
                   w.warp_auto() ? " with the GPU" : "", 100.0 * gn / (gn + rn), 100.0 * rn / (gn + rn));
        const WarpGpuStats& ws = w.warp_gpu_stats();
        if (ws.rounds) { // Phase 8: what the GPU rounds moved, and where their time went
            const double r = static_cast<double>(ws.rounds);
            printf("  warp GPU rounds: %llu, %.0f bodies a round, %.1f MB up and %.1f MB down a round, %.0f warps' "
                   "lanes sent and %.0f fetched;\n"
                   "    ms a round: %.3f into the upload buffer, %.3f on the device, %.3f out of readback, %.3f "
                   "fetching\n",
                   static_cast<unsigned long long>(ws.rounds), ws.records / r, ws.bytes_up / r / 1048576.0,
                   ws.bytes_down / r / 1048576.0, ws.lanes_sent / r, ws.lanes_fetched / r, ws.ms_in / r,
                   ws.ms_gpu / r, ws.ms_out / r, ws.ms_fetch / r);
            if (ws.ms_dev_kernel > 0)
                printf("    on the device's clock: %.3f upload, %.3f kernel, %.3f download\n", ws.ms_dev_up / r,
                       ws.ms_dev_kernel / r, ws.ms_dev_down / r);
            printf("    warp steps a round: %.0f in all, %.0f in the longest body; body records %.0f words on "
                   "average, %llu at most\n",
                   ws.steps / r, ws.steps_longest / r, ws.records ? static_cast<double>(ws.img_words) / ws.records : 0.0,
                   static_cast<unsigned long long>(ws.rec_words_max));
        }
    }
    if (!finish_outputs(run, k, stop, ms / 1000.0))
        return 1;
    if (w.model() == Model::soup) {
        u64 bodies = 0;
        for (u32 n = 0; n < w.node_count(); ++n)
            bodies += w.node_soup(n).body_count();
        printf("nova: tick %llu  hash %016llx  bodies %llu  (%.1f ms)\n",
               static_cast<unsigned long long>(w.tick()), static_cast<unsigned long long>(w.hash()),
               static_cast<unsigned long long>(bodies), ms);
        return 0;
    }
    printf("nova: tick %llu  hash %016llx  units %llu  (%.1f ms)\n",
           static_cast<unsigned long long>(w.tick()), static_cast<unsigned long long>(w.hash()),
           static_cast<unsigned long long>(w.total_units()), ms);
    return 0;
}

// nova resume <snapshot> [--threads N] [--ticks N] [--set key=value]...
// A snapshot continued headlessly up to tick N (the world's tick, not a
// count), printing its hash every replay.checkpoint_every ticks and the
// accelerator and warp notes, for bisecting a divergence between two runs.
// --set changes the snapshot's settings first (warp.device, warp.verify,
// accel.verify, ...). Nothing is written.
bool resume_edit(Config& cfg, void* ctx) {
    return apply_sets(*static_cast<const Args*>(ctx), cfg);
}

int cmd_resume(const Args& a) {
    Array<u8> bytes;
    if (!read_file(a.path, &bytes)) {
        fprintf(stderr, "nova: cannot read '%s'\n", a.path);
        return 1;
    }
    RunKeys defaults;
    apply_priority(defaults);
    Run run;
    TextBuf err;
    if (!run.init_from_snapshot(bytes.data(), bytes.size(), machine_threads(defaults, a.has_threads ? a.threads : 0),
                                &err, &resume_edit, const_cast<Args*>(&a))) {
        fprintf(stderr, "nova: %s\n", err.c_str());
        return 1;
    }
    bytes = Array<u8>();
    const u64 every = key_u64(run.config(), "replay.checkpoint_every");
    const World& w = run.world();
    const u64 ticks = a.has_ticks ? a.ticks : w.tick() + 1000;
    printf("nova: resumed at tick %llu (hash %016llx) on %u threads, to tick %llu\n",
           static_cast<unsigned long long>(w.tick()), static_cast<unsigned long long>(w.hash()), run.threads(),
           static_cast<unsigned long long>(ticks));
    if (run.novelty()) // (docs/analysis-plan.md A2: an observer, not in snapshots)
        printf("nova: the live novelty meter starts a fresh model here (its model isn't in snapshots)\n");
    usize notes_shown = print_notes(w.accel_notes(), 0), warp_notes_shown = print_notes(w.warp_notes(), 0);
    StepProfile prof;
    if (a.profile)
        run.world().set_profile(&prof);
    if (a.dump_round) { // --dump-round T:PATH
        const char* colon = strchr(a.dump_round, ':');
        u64 t = 0;
        if (!colon || !parse_u64(Str(a.dump_round, static_cast<usize>(colon - a.dump_round)), &t)) {
            fprintf(stderr, "nova: --dump-round expects T:PATH\n");
            return 2;
        }
        run.world().set_warp_dump(colon + 1, t);
    }
    const u64 plain0 = w.warp_plain_ticks(), split0 = w.warp_split_ticks(), gn0 = w.warp_gpu_bodies(),
              rn0 = w.warp_ref_bodies();
    const u64 col0 = w.collapses_total(), rec0 = w.recoveries_total(), ticks0 = w.tick();
    const u64 dry0 = w.model() == Model::soup ? w.arrived_dry_sum() : 0;
    const u64 arr0 = w.model() == Model::soup ? w.arrived_sum() : 0;
    SoupTally since = {}; // the soup tallies summed since the resume
    u64 bare0 = 0, dmem0 = 0, ddisk0 = 0;
    if (w.model() == Model::soup)
        for (u32 n = 0; n < w.node_count(); ++n) {
            bare0 += w.node_soup(n).frozen_bare();
            dmem0 += w.node_soup(n).dormant_mem_sum();
            ddisk0 += w.node_soup(n).dormant_disk_sum();
        }
    const double t0 = now_ms();
    while (w.tick() < ticks) {
        run.step();
        if (w.model() == Model::soup) {
            const u64* t = reinterpret_cast<const u64*>(&w.last_soup_tick());
            u64* s = reinterpret_cast<u64*>(&since);
            for (usize i = 0; i < sizeof(SoupTally) / sizeof(u64); ++i)
                s[i] += t[i];
        }
        print_tick_hash(a, w);
        run.drop_events();
        if (w.accel_notes().n > notes_shown)
            notes_shown = print_notes(w.accel_notes(), notes_shown);
        if (w.warp_notes().n > warp_notes_shown)
            warp_notes_shown = print_notes(w.warp_notes(), warp_notes_shown);
        if (w.warp_failed()) {
            printf("nova: stopped (warp_device_error) at tick %llu\n", static_cast<unsigned long long>(w.tick()));
            return 1;
        }
        if (every && w.tick() % every == 0) {
            printf("hash %llu %016llx\n", static_cast<unsigned long long>(w.tick()),
                   static_cast<unsigned long long>(w.hash()));
            fflush(stdout);
        }
    }
    if (a.profile && prof.ticks) { // ms a tick: the parts, and what auto chose
        const double per = 1000.0 / static_cast<double>(prof.ticks);
        const double barrier = prof.signals + prof.departures + prof.delivery + prof.links + prof.lifecycle +
                               prof.tally + prof.ports;
        const u64 gn = w.warp_gpu_bodies() - gn0, rn = w.warp_ref_bodies() - rn0;
        printf("nova: profile over %llu ticks, ms a tick: update %.3f, warps' CPU passes %.3f, warp rounds %.3f, "
               "accelerators %.3f, barrier %.3f, after %.3f; nodes' reference %.3f thread-ms; %llu ticks with the "
               "GPU, %llu without; %.1f%% of body-rounds on the GPU\n",
               static_cast<unsigned long long>(prof.ticks), prof.update * per, prof.warp_cpu * per,
               prof.warp_gpu * per, prof.accel * per, barrier * per, prof.after * per, prof.n_ref * per,
               static_cast<unsigned long long>(w.warp_split_ticks() - split0),
               static_cast<unsigned long long>(w.warp_plain_ticks() - plain0),
               gn + rn ? 100.0 * static_cast<double>(gn) / static_cast<double>(gn + rn) : 0.0);
        printf("nova: warp phases, wall ms a tick: first halves %.3f, pack %.3f, split %.3f, unpack %.3f, host lanes "
               "%.3f, host instructions %.3f, second halves %.3f, later rounds' passes %.3f (split: choosing %.3f, "
               "fetching %.3f); the slowest node: first half %.3f, pack %.3f, second half %.3f\n",
               prof.w_first * per, prof.w_pack * per, prof.w_split * per, prof.w_unpack * per,
               prof.w_hostlanes * per, prof.w_host * per, prof.w_second * per, prof.w_later * per,
               prof.w_choose * per, prof.w_fetch * per, prof.w_first_max * per, prof.w_pack_max * per,
               prof.w_second_max * per);
        printf("nova: nodes' parts, thread-ms a tick: first %.3f (bodies %.3f), pack %.3f, reference %.3f, unpack "
               "%.3f, host %.3f, second %.3f; barrier: signals %.3f, departures %.3f, delivery %.3f, links %.3f, "
               "lifecycle %.3f, tallies %.3f, ports %.3f\n",
               prof.n_first * per, prof.n_bodies * per, prof.n_pack * per, prof.n_ref * per, prof.n_unpack * per,
               prof.n_host * per, prof.n_second * per, prof.signals * per, prof.departures * per,
               prof.delivery * per, prof.links * per, prof.lifecycle * per, prof.tally * per, prof.ports * per);
        const WarpGpuStats& ws = w.warp_gpu_stats();
        if (ws.rounds) {
            const double r = static_cast<double>(ws.rounds);
            printf("nova: the warp kernel: %s\n", w.warp_kernel());
            printf("nova: GPU rounds %llu: %.0f warp steps a round, %.0f in the longest body; on the device's clock "
                   "%.3f ms kernel, %.3f upload, %.3f download a round; %.0f warps' lanes sent a round; the CPU "
                   "threads' share %.3f ms a round; model %.3f us a GPU step, %.4f us a reference step, limit %u\n",
                   static_cast<unsigned long long>(ws.rounds), ws.steps / r, ws.steps_longest / r, ws.ms_dev_kernel / r,
                   ws.ms_dev_up / r, ws.ms_dev_down / r, ws.lanes_sent / r, w.warp_ref_wall() * 1000.0 / r,
                   w.warp_model_gstep() * 1e6, w.warp_model_hstep() * 1e6, w.warp_limit());
            printf("nova: lanes back a round: %.0f warps gathered with the round, %.0f fetched in %.2f fetches "
                   "(%.3f ms)\n",
                   ws.lanes_gathered / r, ws.lanes_fetched / r, ws.fetches / r, ws.ms_fetch / r);
            printf("nova: a GPU round's host side, wall ms: %.3f the records' table, %.3f into the upload buffer, %.3f "
                   "recording the commands, %.3f submitted to back, %.3f out of readback; %.1f MB up and %.1f MB down\n",
                   ws.ms_prep / r, ws.ms_in / r, ws.ms_post / r, ws.ms_gpu / r, ws.ms_out / r,
                   ws.bytes_up / r / 1048576.0, ws.bytes_down / r / 1048576.0);
        }
    }
    write_node_hashes(a, w);
    dump_node(a, w);
    printf("nova: tick %llu  hash %016llx  (%.1f ms)  warp verified rounds %llu, mismatches %llu; accel mismatches "
           "%llu\n",
           static_cast<unsigned long long>(w.tick()), static_cast<unsigned long long>(w.hash()), now_ms() - t0,
           static_cast<unsigned long long>(w.warp_verified()), static_cast<unsigned long long>(w.warp_mismatches()),
           static_cast<unsigned long long>(w.accel_mismatches()));
    if (w.model() == Model::soup) {
        const u64 nc = w.collapses_total() - col0;
        printf("nova: since tick %llu: %llu collapses (worn out or surged; %.0f ticks old and %.0f bodies each on "
               "average), "
               "%llu recoveries; %llu live nodes of %u\n",
               static_cast<unsigned long long>(ticks0), static_cast<unsigned long long>(nc),
               nc ? static_cast<double>(w.collapse_age_sum()) / static_cast<double>(nc) : 0.0,
               nc ? static_cast<double>(w.collapse_bodies_sum()) / static_cast<double>(nc) : 0.0,
               static_cast<unsigned long long>(w.recoveries_total() - rec0),
               static_cast<unsigned long long>(w.live_nodes()), w.node_count());
        u64 bare = 0, dmem = 0, ddisk = 0, resting = 0;
        for (u32 n = 0; n < w.node_count(); ++n) {
            bare += w.node_soup(n).frozen_bare();
            dmem += w.node_soup(n).dormant_mem_sum();
            ddisk += w.node_soup(n).dormant_disk_sum();
            resting += w.node_soup(n).resting_count();
        }
        bare -= bare0;
        dmem -= dmem0;
        ddisk -= ddisk0;
        printf("nova: since tick %llu: %llu bodies born, %llu died (%llu halted, %llu frozen, %llu without a millicore), "
               "%llu swallowed whole\n",
               static_cast<unsigned long long>(ticks0), static_cast<unsigned long long>(since.births),
               static_cast<unsigned long long>(since.deaths), static_cast<unsigned long long>(since.died_halted),
               static_cast<unsigned long long>(since.died_frozen), static_cast<unsigned long long>(since.died_worn),
               static_cast<unsigned long long>(since.engulfs));
        if (w.soup_params().dormancy) {
            const u64 ds = since.dormant_started;
            printf("nova: since tick %llu: %llu went dormant (reserves %.0f codons of memory and %.0f of disk each on "
                   "average; %llu frozen with none died), %llu woke, %llu expired, %llu eaten, %llu hops; now %llu "
                   "drifting and %llu resting\n",
                   static_cast<unsigned long long>(ticks0), static_cast<unsigned long long>(ds),
                   ds ? static_cast<double>(dmem) / ds : 0.0, ds ? static_cast<double>(ddisk) / ds : 0.0,
                   static_cast<unsigned long long>(bare), static_cast<unsigned long long>(since.dormant_woke),
                   static_cast<unsigned long long>(since.dormant_expired),
                   static_cast<unsigned long long>(since.dormant_eaten),
                   static_cast<unsigned long long>(since.dormant_hops), static_cast<unsigned long long>(w.drifting()),
                   static_cast<unsigned long long>(resting));
        }
        const u64 arr = w.arrived_sum() - arr0, dry = w.arrived_dry_sum() - dry0;
        printf("nova: since tick %llu: %llu arrivals, %llu of them (%.0f%%) with no free millicore (dead in their "
               "first sweep)\n",
               static_cast<unsigned long long>(ticks0), static_cast<unsigned long long>(arr),
               static_cast<unsigned long long>(dry), arr ? 100.0 * static_cast<double>(dry) / arr : 0.0);
        if (w.disasters()) { // docs/disasters-plan.md X7
            const auto u = [](u64 v) { return static_cast<unsigned long long>(v); };
            printf("nova: since tick %llu: disasters: %llu surges (%llu nodes collapsed, %llu bodies), %llu crashes "
                   "(%llu deleted, %llu restarted), %llu burnouts (%llu cores offline, %llu dormant of which %llu "
                   "graced, %llu died), %llu disk failures (%llu codons), %llu storms formed, %llu strikes (%llu "
                   "codons)\n",
                   u(ticks0), u(since.surges), u(since.surge_nodes), u(since.surge_deaths), u(since.crashes),
                   u(since.crash_deaths), u(since.crash_restarts), u(since.burnouts), u(since.cores_burnt),
                   u(since.burnout_dormant), u(since.burnout_graced), u(since.burnout_deaths), u(since.disk_failures),
                   u(since.disk_lost), u(since.storms_formed), u(since.strikes), u(since.strike_codons));
            printf("nova: since tick %llu: %llu symptom episodes (%llu false alarms), %llu spurious faults, %llu "
                   "static flips, %llu sounds; now %llu cores offline, %llu nodes rebooting, %u storms out, %u "
                   "episodes under way\n",
                   u(ticks0), u(since.episodes), u(since.false_alarms), u(since.spurious_faults),
                   u(since.static_flips), u(since.sounds), u(w.cores_offline()), u(w.rebooting_count()),
                   w.storm_count(), w.episode_count());
        }
    }
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
    if (log.version() < kReplayRaysBefore) // (docs/results/integration.md; legacy.h)
        printf("nova: a version %u log: settings it doesn't name take the defaults it was written with (legacy.h)\n",
               log.version());
    ReplayResult r;
    double t0 = now_ms();
    bool ok = replay_verify(log, threads, &r);
    double ms = now_ms() - t0;
    if (!ok) {
        fprintf(stderr, "nova: replay FAILED: %s\n", r.msg);
        return 1;
    }
    printf("nova: replay OK: %llu checkpoints match, %llu knob events, %llu observer events (%.1f ms)\n",
           static_cast<unsigned long long>(r.checkpoints_checked),
           static_cast<unsigned long long>(log.knob_count()),
           static_cast<unsigned long long>(r.events_checked), ms);
    return 0;
}

} // namespace

int nova_main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "devices") == 0)
        return cmd_devices();
    if (argc == 3 && strcmp(argv[1], "bench") == 0 && strcmp(argv[2], "costs") == 0)
        return cmd_bench_costs();
    if (argc >= 3 && strcmp(argv[1], "bench") == 0 && strcmp(argv[2], "warp") == 0)
        return cmd_bench_warp(argc, argv);
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "costs") == 0) { // Phase 9: a cost table
        CostTable t;
        if (!builtin_costs(Str(argc == 3 ? argv[2] : "home_v1"), &t)) {
            fprintf(stderr, "nova costs: unknown table '%s' (classic or home_v1)\n", argv[2]);
            return 2;
        }
        TextBuf out;
        describe_costs(t, out);
        fwrite(out.view().p, 1, out.size(), stdout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "view") == 0)
        return cmd_view(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "land") == 0)
        return cmd_land(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "keys") == 0)
        return cmd_keys(argc, argv);
    // Learning from the runs (docs/analysis-plan.md): analyse_cmd.cpp.
    if (argc >= 2 && strcmp(argv[1], "analyse") == 0)
        return cmd_analyse(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "search") == 0)
        return cmd_search(argc, argv);
    Args a;
    if (!parse_args(argc, argv, &a))
        return usage();
    if (strcmp(a.mode, "run") == 0)
        return cmd_run(a);
    if (strcmp(a.mode, "replay") == 0)
        return cmd_replay(a);
    if (strcmp(a.mode, "resume") == 0)
        return cmd_resume(a);
    if (strcmp(a.mode, "bench") == 0 && strcmp(a.path, "devices") == 0)
        return cmd_bench_devices(a.has_threads ? static_cast<u32>(a.threads) : 0);
    if (strcmp(a.mode, "bench") == 0) {
        double seconds = 0;
        if (!parse_f64(a.path, &seconds) || !(seconds > 0 && seconds <= 3600))
            return usage();
        return cmd_bench(seconds, a.has_threads ? static_cast<u32>(a.threads) : 0);
    }
    return usage();
}

// Started by a double click in Explorer, nova gets a console window of its
// own, which closes the moment it exits: what it printed (the usage, an error,
// a run's result) would vanish unread. So when no other process shares the
// console, it waits for Enter first. From a terminal it never waits.
int main(int argc, char** argv) {
    const int rc = nova_main(argc, argv);
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1) {
        fflush(stdout);
        fprintf(stderr, "\nnova: press Enter to close this window\n");
        getchar();
    }
    return rc;
}
