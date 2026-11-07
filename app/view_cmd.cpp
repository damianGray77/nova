// nova view -- the analysis UI's Win32 shell (Phase 6, design.md 6.12), and
// the setup page's (docs/config-plan.md C2-C5).
//
//   nova view [--setup <config>] [--set key=value]... [--threads N] [--group NAME]
//             [--render out.png [--size WxH]]
//   (--group NAME also opens a run's or a finished run's settings page there)
//   nova view <config | snapshot> [--threads N] [--set key=value]... [--no-output]
//             [--ticks N] [--sample N] [--color NAME] [--render out.png [--size WxH]]
//   nova view --run <run directory> [--render out.png [--window K] [--size WxH]]
//
// With no config, or --setup <config>, the window opens on the setup page:
// every setting, set before a run begins (the default preset, or that file).
// Start writes every setting to the run's folder as run.cfg and starts it,
// in the window (watched, today's panels) or headless (`nova run run.cfg` in
// a console of its own, with a stop file). A config runs live at once,
// writing the outputs it names (a watched run is a real run; --no-output turns
// them all off). A snapshot continues live from its tick with its outputs
// off. --run browses a finished run's files without simulating. --render
// draws one frame into a PNG instead of opening a window (after --ticks N
// ticks of a live run; the page itself with --setup or no config).
//
// A run's settings are fixed from its start (C3, Damian): the toolbar has no
// knobs, and the run is locked (Run::lock_knobs). Its settings show on the
// setup page, read-only (the toolbar's Settings button).
//
// Threads (plan C2): a sim thread owns the Run and steps it; the UI thread
// owns the window. After a step the sim thread builds a view frame when the
// UI has asked for one, between ticks, and carries on. Run controls go from
// the UI into a queue that the sim thread applies between ticks: watching
// changes nothing.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "canvas.h"
#include "nova/keys.h"
#include "nova/png.h"
#include "nova/replay.h"
#include "nova/snapshot.h"
#include "nova/view.h"
#include "setup.h"
#include "viewer.h"

namespace nova {

namespace {

struct ViewArgs {
    const char* source = nullptr;  // a config or a snapshot, run live
    const char* setup = nullptr;   // --setup <config>: the setup page on it
    const char* group = nullptr;   // --group NAME: the page's group to show first
    bool page = false;             // the setup page (no config, or --setup)
    const char* run_dir = nullptr; // --run
    const char* render = nullptr;  // --render out.png
    bool has_ticks = false, has_threads = false, no_output = false, has_window = false;
    u64 ticks = 0, threads = 0, window = 0;
    u32 width = 1600, height = 1000;
    u32 sample = 100;
    u32 color = kColorBodies; // --color: what the map colours nodes by
    const char* sets[256] = {};
    int set_count = 0;
};

bool parse_view_args(int argc, char** argv, ViewArgs* a) {
    for (int i = 2; i < argc; ++i) {
        const char* s = argv[i];
        auto next = [&](const char** out) {
            if (i + 1 >= argc)
                return false;
            *out = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (strcmp(s, "--no-output") == 0) {
            a->no_output = true;
        } else if (strcmp(s, "--run") == 0) {
            if (!next(&a->run_dir))
                return false;
        } else if (strcmp(s, "--setup") == 0) {
            if (!next(&a->setup))
                return false;
        } else if (strcmp(s, "--group") == 0) {
            if (!next(&a->group))
                return false;
        } else if (strcmp(s, "--render") == 0) {
            if (!next(&a->render))
                return false;
        } else if (strcmp(s, "--ticks") == 0) {
            if (!next(&v) || !parse_u64(v, &a->ticks))
                return false;
            a->has_ticks = true;
        } else if (strcmp(s, "--threads") == 0) {
            if (!next(&v) || !parse_u64(v, &a->threads) || a->threads > 1024)
                return false;
            a->has_threads = true;
        } else if (strcmp(s, "--window") == 0) {
            if (!next(&v) || !parse_u64(v, &a->window))
                return false;
            a->has_window = true;
        } else if (strcmp(s, "--sample") == 0) {
            u64 n = 0;
            if (!next(&v) || !parse_u64(v, &n) || n == 0 || n > 1000000)
                return false;
            a->sample = static_cast<u32>(n);
        } else if (strcmp(s, "--size") == 0) {
            if (!next(&v))
                return false;
            const Str sz = v;
            const usize x = sz.find('x');
            u64 w = 0, h = 0;
            if (x >= sz.n || !parse_u64(sz.sub(0, x), &w) || !parse_u64(sz.sub(x + 1, sz.n - x - 1), &h) || w < 320 ||
                h < 240 || w > 8192 || h > 8192)
                return false;
            a->width = static_cast<u32>(w);
            a->height = static_cast<u32>(h);
        } else if (strcmp(s, "--color") == 0) {
            if (!next(&v))
                return false;
            a->color = kColorCount;
            for (u32 c = 0; c < kColorCount; ++c) {
                // names may have spaces ("free CPU"): match ignoring them and case
                const char* name = color_by_name(c);
                const char* p = v;
                bool same = true;
                for (; *name && same; ++name) {
                    if (*name == ' ')
                        continue;
                    const char x = *name >= 'A' && *name <= 'Z' ? static_cast<char>(*name + 32) : *name;
                    const char y = *p >= 'A' && *p <= 'Z' ? static_cast<char>(*p + 32) : *p;
                    same = x == y;
                    ++p;
                }
                if (same && *p == '\0')
                    a->color = c;
            }
            if (a->color == kColorCount)
                return false;
        } else if (strcmp(s, "--set") == 0) {
            if (!next(&v) || a->set_count == 256)
                return false;
            a->sets[a->set_count++] = v;
        } else if (s[0] == '-' || a->source) {
            return false;
        } else {
            a->source = s;
        }
    }
    // One of: a config or snapshot, a finished run, or the setup page.
    const int what = (a->source != nullptr) + (a->run_dir != nullptr) + (a->setup != nullptr);
    a->page = a->setup != nullptr || what == 0;
    return what <= 1;
}

bool is_snapshot(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    char magic[8] = {};
    const usize n = fread(magic, 1, 8, f);
    fclose(f);
    return n == 8 && memcmp(magic, "NOVASNAP", 8) == 0;
}

void note(char* out, usize n, const char* fmt_a, const char* b, const char* c) { snprintf(out, n, fmt_a, b, c); }

// The setup page's check reads what nova run reads before the world, too.
void front_end_reader(Config& cfg) {
    RunKeys k;
    read_run_keys(cfg, &k);
}

bool exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

// Creates a folder and any missing parents.
bool make_dirs(const char* path) {
    char p[MAX_PATH];
    const usize n = strlen(path);
    if (n == 0 || n >= sizeof p)
        return false;
    memcpy(p, path, n + 1);
    for (usize i = 1; i < n; ++i) {
        if (p[i] != '\\' && p[i] != '/')
            continue;
        if (p[i - 1] == ':')
            continue; // a drive's root
        p[i] = '\0';
        CreateDirectoryA(p, nullptr);
        p[i] = '\\';
    }
    CreateDirectoryA(p, nullptr);
    const DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Every match of a pattern, one path a line: files (configs\*.cfg) or folders (runs\*).
void list_paths(const char* dir, const char* pattern, bool folders, TextBuf* out) {
    char glob[MAX_PATH];
    snprintf(glob, sizeof glob, "%s\\%s", dir, pattern);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir != folders || strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        out->append(dir);
        out->append_char('\\');
        out->append(fd.cFileName);
        out->append_char('\n');
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// The default preset: configs\landscape.cfg (the README's quick start), from
// the current folder or the repository beside build*\bin\nova.exe.
bool default_preset(char* out, usize n) {
    if (exists("configs\\landscape.cfg")) {
        snprintf(out, n, "configs\\landscape.cfg");
        return true;
    }
    char exe[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    for (int up = 0; up < 3; ++up) { // ...\bin\nova.exe -> ...\bin -> ... -> the repository
        char* slash = strrchr(exe, '\\');
        if (!slash)
            return false;
        *slash = '\0';
        char probe[MAX_PATH + 32];
        snprintf(probe, sizeof probe, "%s\\configs\\landscape.cfg", exe);
        if (exists(probe)) {
            snprintf(out, n, "%s", probe);
            return true;
        }
    }
    return false;
}

// --- the live run's shared state ------------------------------------------------

const UINT kMsgFrame = WM_APP + 1;

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    Array<ViewCommand> cmds;
    ViewRequest req;
    bool want = true;           // the UI wants a frame
    bool quit = false;
    ViewFrame* ready = nullptr; // a built frame the UI hasn't taken yet
    bool paused = false, finished = false;
    double speed = 0;
    char note[200] = {};
    HWND hwnd = nullptr;
};

struct App {
    // a live run
    Run* run = nullptr;
    RunKeys* keys = nullptr;
    bool outputs = false;
    u64 tick_limit = 0; // pause after this many ticks (0 = none)
    double seconds = 0; // ... or this much wall time
    i64 after = -1;     // ... or this many ticks after the first replicator
    ViewSession* session = nullptr;
    ViewFrame frames[2];
    Shared sh;
    HANDLE thread = nullptr;
    u32 sample = 100;
    bool has_threads = false;
    u64 threads = 0;
    // a finished run
    RunArchive* archive = nullptr;
    ViewFrame archive_frame;
    // the UI
    Viewer* viewer = nullptr;
    SetupPage page;
    bool on_page = false; // the setup page is showing, not the panels
    Canvas canvas;
    UiInput input;
    RunStatus status;
    HWND hwnd = nullptr;
    double rate_t = 0;
    u64 rate_tick = 0, rate_instr = 0;
    char source[260] = {};
    bool tracking = false;
};

App* g_app = nullptr;

void set_note(Shared& sh, const char* text) {
    AcquireSRWLockExclusive(&sh.lock);
    snprintf(sh.note, sizeof sh.note, "%s", text);
    ReleaseSRWLockExclusive(&sh.lock);
}

// The end of a watched run: every output finished, as `nova run` does.
void finish_run(App& app, const char* stop, double seconds) {
    app.run->finish();
    if (app.outputs)
        finish_outputs(*app.run, *app.keys, stop, seconds);
}

DWORD WINAPI sim_main(void* param) {
    App& app = *static_cast<App*>(param);
    Shared& sh = app.sh;
    Run& run = *app.run;
    bool paused = false, finished = false, step_once = false;
    double speed = 0;
    const double t0 = now_ms();
    double last_frame = 0, next_due = t0;
    const u64 start_tick = run.world().tick();
    int next = 0;
    Array<ViewCommand> cmds;
    ViewRequest last_req;
    char msg[200];
    for (;;) {
        ViewRequest req;
        bool want;
        AcquireSRWLockExclusive(&sh.lock);
        if (sh.quit) {
            ReleaseSRWLockExclusive(&sh.lock);
            break;
        }
        cmds.clear();
        Array<ViewCommand> tmp = move(sh.cmds);
        sh.cmds = move(cmds);
        cmds = move(tmp);
        req = sh.req;
        want = sh.want && !sh.ready;
        ReleaseSRWLockExclusive(&sh.lock);

        for (usize i = 0; i < cmds.size(); ++i) {
            const ViewCommand& c = cmds[i];
            switch (c.kind) {
            case kCmdPause:
                paused = true;
                break;
            case kCmdResume:
                if (!finished) {
                    paused = false;
                    next_due = now_ms();
                }
                break;
            case kCmdStep:
                paused = true;
                step_once = !finished;
                break;
            case kCmdSpeed:
                speed = c.value;
                next_due = now_ms();
                break;
            case kCmdSnapshot: {
                TextBuf path;
                path.append(app.keys && app.keys->snap.size() ? app.keys->snap.view() : Str("nova_view.snapshot"));
                path.append_char('.');
                path.append_u64(run.world().tick());
                ByteWriter bytes;
                run.snapshot(bytes);
                ensure_parent_dir(path.c_str());
                if (write_file(path.c_str(), bytes.data(), bytes.size()))
                    note(msg, sizeof msg, "snapshot written: %s%s", path.c_str(), "");
                else
                    note(msg, sizeof msg, "cannot write snapshot %s%s", path.c_str(), "");
                set_note(sh, msg);
                break;
            }
            case kCmdStop:
                if (!finished) {
                    finish_run(app, "view_stop", (now_ms() - t0) / 1000.0);
                    finished = true;
                    paused = true;
                    set_note(sh, app.outputs ? "run stopped: its outputs are written" : "run stopped");
                }
                break;
            default: // (no knobs: a run's settings are fixed from its start, config-plan C3)
                break;
            }
        }

        bool ran = false;
        double now = now_ms();
        if (!finished && (!paused || step_once) && (speed <= 0 || now >= next_due || step_once)) {
            run.step();
            app.session->after_step();
            ran = true;
            if (speed > 0) {
                next_due += 1000.0 / speed;
                if (next_due < now - 200)
                    next_due = now - 200; // don't race to catch up after a stall
            }
            step_once = false;
            const World& w = run.world();
            const u64 done = w.tick() - start_tick;
            const Detection& d = run.detection();
            const char* why = nullptr;
            if (app.tick_limit && done == app.tick_limit)
                why = "reached the tick limit (run.ticks or --ticks): paused";
            else if (app.seconds > 0 && now - t0 >= app.seconds * 1000.0 && !paused)
                why = "reached run.seconds: paused";
            else if (app.after >= 0 && d.found && w.tick() == d.tick + 1 + static_cast<u64>(app.after))
                why = "reached run.after_replicator: paused";
            else if (w.accel_mismatches())
                why = "a device result differed from the CPU reference (accel.verify): paused";
            else if (run.snapshot_error().n)
                why = "a numbered snapshot could not be written: paused";
            if (why && !paused) {
                paused = true;
                set_note(sh, why);
            }
        }
        now = now_ms();
        // Frames: about 30 a second while running; while paused, when the
        // request changes (a new selection), after a command, or 4 a second.
        const bool still = paused || finished;
        const bool changed = req.node != last_req.node || req.body != last_req.body ||
                             req.serial != last_req.serial || req.strip != last_req.strip ||
                             req.genotype != last_req.genotype;
        if (want && (still ? (changed || !cmds.empty() || ran || now - last_frame >= 250.0) : now - last_frame >= 33.0)) {
            if (still)
                app.session->flush_sample();
            req.hash = still;
            last_req = req;
            ViewFrame* f = &app.frames[next];
            next ^= 1;
            app.session->build(req, f);
            AcquireSRWLockExclusive(&sh.lock);
            sh.ready = f;
            sh.want = false;
            sh.paused = paused;
            sh.finished = finished;
            sh.speed = speed;
            ReleaseSRWLockExclusive(&sh.lock);
            PostMessageW(sh.hwnd, kMsgFrame, 0, 0);
            last_frame = now;
        }
        if (!ran) {
            // Idle (paused, stopped, or waiting for the speed limit): sleep
            // until a command or a new request wakes us, the next tick is due,
            // or a paused frame is.
            double wait = 50;
            if (!paused && !finished && speed > 0)
                wait = next_due - now_ms();
            if (want && (paused || finished)) {
                const double frame_due = last_frame + 250.0 - now_ms();
                wait = frame_due < wait ? frame_due : wait;
            }
            const DWORD ms = wait < 1 ? 1 : (wait > 50 ? 50 : static_cast<DWORD>(wait));
            AcquireSRWLockExclusive(&sh.lock);
            const bool fresh = sh.req.node != last_req.node || sh.req.body != last_req.body ||
                               sh.req.serial != last_req.serial || sh.req.strip != last_req.strip ||
                               sh.req.genotype != last_req.genotype;
            if (!sh.quit && sh.cmds.empty() && !(fresh && sh.want && !sh.ready))
                SleepConditionVariableSRW(&sh.wake, &sh.lock, ms, 0);
            ReleaseSRWLockExclusive(&sh.lock);
        }
    }
    if (!finished)
        finish_run(app, "view_close", (now_ms() - t0) / 1000.0);
    return 0;
}

// --- starting runs ------------------------------------------------------------------

// The settings a run started with, on the setup page, read-only.
void page_from_run(App& app, u32 mode, const char* title) {
    TextBuf t;
    app.run->config().write(t);
    TextBuf err;
    app.page.load_text(t.view(), title, &err);
    app.page.set_mode(mode, title);
}

// A live run from a config file, as `nova view <config>` starts it (and the
// setup page's Start in window, on its run.cfg). Prints nothing on success;
// on failure, why (in err).
bool start_live(App& app, const char* path, const char* const* sets, int nsets, bool outputs, bool has_ticks,
                u64 ticks, TextBuf* err) {
    Run* run = new Run;
    RunKeys* keys = new RunKeys;
    Config& cfg = run->config();
    Config::ParseError pe;
    auto fail = [&](const char* what) {
        err->append(what);
        delete run;
        delete keys;
        return false;
    };
    if (!cfg.load_file(path, &pe)) {
        TextBuf t;
        t.append(path);
        t.append(": ");
        t.append(pe.msg);
        return fail(t.c_str());
    }
    for (int i = 0; i < nsets; ++i)
        if (!apply_set(cfg, sets[i]))
            return fail("a --set is malformed");
    read_run_keys(cfg, keys);
    if (app.has_threads)
        keys->threads = app.threads;
    apply_priority(*keys);
    TextBuf why;
    if (!run->init(machine_threads(*keys, keys->threads), &why))
        return fail(why.c_str());
    print_budget_notes(*run);
    run->lock_knobs(); // a run's settings are fixed from its start (docs/config-plan.md C3)
    key_accept_known(cfg); // a key the registry knows that nothing read is no typo (run.cfg names them all)
    Str unused;
    if (cfg.first_unused(&unused)) {
        TextBuf t;
        t.append("config key '");
        t.append(unused);
        t.append("' is not used by anything (typo?)");
        return fail(t.c_str());
    }
    if (outputs && !open_outputs(*run, *keys))
        return fail("cannot open the run's outputs (the console says why)");
    app.run = run;
    app.keys = keys;
    app.outputs = outputs;
    app.tick_limit = has_ticks ? ticks : keys->ticks;
    app.seconds = keys->seconds;
    app.after = keys->after;
    run->keep_genebank(true);
    if (!app.session)
        app.session = new ViewSession;
    app.session->attach(run, app.sample);
    app.status.live = true;
    return true;
}

// Starts the sim thread (the window exists).
bool begin_sim(App& app) {
    if (!app.run || app.thread || !app.hwnd)
        return app.thread != nullptr;
    app.sh.hwnd = app.hwnd;
    app.thread = CreateThread(nullptr, 0, sim_main, &app, 0, nullptr);
    if (!app.thread)
        return false;
    SetTimer(app.hwnd, 1, 100, nullptr); // a fallback poll; frames arrive by message
    return true;
}

void end_sim(App& app) {
    if (!app.thread)
        return;
    AcquireSRWLockExclusive(&app.sh.lock);
    app.sh.quit = true;
    ReleaseSRWLockExclusive(&app.sh.lock);
    WakeConditionVariable(&app.sh.wake);
    WaitForSingleObject(app.thread, INFINITE);
    CloseHandle(app.thread);
    app.thread = nullptr;
}

// `nova run <folder>\run.cfg --stop-file <folder>\stop` in a console of its own.
bool launch_headless(App& app, const char* folder, TextBuf* err) {
    char exe[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        err->append("cannot find nova.exe itself");
        return false;
    }
    char stop[MAX_PATH + 8];
    snprintf(stop, sizeof stop, "%s\\stop", folder);
    DeleteFileA(stop); // (a stale one would stop it at once)
    char cmd[MAX_PATH * 3 + 64];
    int n = snprintf(cmd, sizeof cmd, "\"%s\" run \"%s\\run.cfg\" --stop-file \"%s\\stop\"", exe, folder, folder);
    if (app.has_threads)
        snprintf(cmd + n, sizeof cmd - n, " --threads %llu", static_cast<unsigned long long>(app.threads));
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        err->append("cannot start nova run (CreateProcess failed)");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

char g_configs[MAX_PATH] = "configs"; // where the presets are (beside the default preset)

void refresh_lists(App& app) {
    TextBuf presets, runs;
    list_paths(g_configs, "*.cfg", false, &presets);
    list_paths("runs", "*", true, &runs);
    app.page.set_presets(presets.view());
    app.page.set_run_folders(runs.view());
}

void set_title(App& app) {
    if (!app.hwnd)
        return;
    char title[320];
    snprintf(title, sizeof title, "Nova: setup - %s", app.page.source());
    SetWindowTextA(app.hwnd, title);
}

// What the setup page asked for, after a frame.
void page_requests(App& app) {
    Array<SetupRequest>& reqs = app.page.requests();
    for (usize i = 0; i < reqs.size(); ++i) {
        const SetupRequest& r = reqs[i];
        char msg[400];
        switch (r.kind) {
        case kReqBack:
            if (app.run || app.archive)
                app.on_page = false;
            break;
        case kReqStopHeadless: {
            char stop[MAX_PATH];
            snprintf(stop, sizeof stop, "%s\\stop", r.folder);
            if (write_file(stop, "", 0)) {
                snprintf(msg, sizeof msg, "asked the headless run in %s to stop (its outputs are finished)", r.folder);
                app.page.set_note(msg);
                app.page.set_headless("");
            } else {
                app.page.set_note("cannot write the stop file", true);
            }
            break;
        }
        case kReqStartWindow:
        case kReqStartHeadless: {
            const bool headless = r.kind == kReqStartHeadless;
            if (!headless && app.run) {
                app.page.set_note("a run is open in this window already: start this one headless", true);
                break;
            }
            if (!app.page.can_start())
                break;
            if (!make_dirs(r.folder)) {
                snprintf(msg, sizeof msg, "cannot create the folder %s", r.folder);
                app.page.set_note(msg, true);
                break;
            }
            char cfg_path[MAX_PATH];
            snprintf(cfg_path, sizeof cfg_path, "%s\\run.cfg", r.folder);
            TextBuf text;
            app.page.run_cfg_text(r.folder, text);
            if (!write_file(cfg_path, text.view().p, text.size())) {
                snprintf(msg, sizeof msg, "cannot write %s", cfg_path);
                app.page.set_note(msg, true);
                break;
            }
            TextBuf err;
            if (headless) {
                if (launch_headless(app, r.folder, &err)) {
                    snprintf(msg, sizeof msg, "started headless: nova run %s (Esc in its console, or Stop it)", cfg_path);
                    app.page.set_note(msg);
                    app.page.set_headless(r.folder);
                    refresh_lists(app);
                    app.page.default_folder(); // the next run gets a folder of its own
                } else {
                    app.page.set_note(err.c_str(), true);
                    DeleteFileA(cfg_path);
                }
                break;
            }
            // In the window: a finished run's files make way for the new run.
            if (app.archive) {
                delete app.archive;
                app.archive = nullptr;
            }
            if (start_live(app, cfg_path, nullptr, 0, true, false, 0, &err)) {
                delete app.viewer;
                app.viewer = new Viewer;
                app.status = RunStatus{};
                app.status.live = true;
                app.rate_t = 0;
                snprintf(app.source, sizeof app.source, "%s", cfg_path);
                snprintf(app.status.title, sizeof app.status.title, "%s", r.folder);
                page_from_run(app, kSetupLive, r.folder);
                app.page.set_info("what this run started with (its run.cfg); fixed for the whole run");
                if (!begin_sim(app)) {
                    app.page.set_note("cannot start the simulation thread", true);
                    break;
                }
                app.on_page = false;
            } else {
                app.page.set_note(err.c_str(), true);
                DeleteFileA(cfg_path);
                RemoveDirectoryA(r.folder); // (only if empty)
            }
            break;
        }
        default:
            break;
        }
    }
    reqs.clear();
}

// --- the window -------------------------------------------------------------------

void push_commands(App& app) {
    Array<ViewCommand>& cmds = app.viewer->commands();
    for (usize i = 0; i < cmds.size(); ++i)
        if (cmds[i].kind == kCmdSettings)
            app.on_page = true; // the run's settings, read-only
    if (app.archive) {
        for (usize i = 0; i < cmds.size(); ++i) {
            if (cmds[i].kind != kCmdWindow)
                continue;
            const u32 n = app.archive->windows();
            const u32 w = static_cast<u32>(cmds[i].value);
            if (w < n && w != app.status.window) {
                app.status.window = w;
                app.archive->build(w, app.viewer->request(), &app.archive_frame);
                app.viewer->take(app.archive_frame);
                InvalidateRect(app.hwnd, nullptr, FALSE);
            }
        }
        cmds.clear();
        return;
    }
    if (!app.run) {
        cmds.clear();
        return;
    }
    AcquireSRWLockExclusive(&app.sh.lock);
    for (usize i = 0; i < cmds.size(); ++i)
        if (cmds[i].kind != kCmdSettings)
            app.sh.cmds.push(cmds[i]);
    app.sh.req = app.viewer->request();
    ReleaseSRWLockExclusive(&app.sh.lock);
    WakeConditionVariable(&app.sh.wake);
    cmds.clear();
}

void take_frame(App& app) {
    if (!app.run)
        return;
    ViewFrame* f = nullptr;
    AcquireSRWLockExclusive(&app.sh.lock);
    f = app.sh.ready;
    app.sh.ready = nullptr;
    app.status.paused = app.sh.paused;
    app.status.finished = app.sh.finished;
    app.status.speed = app.sh.speed;
    snprintf(app.status.note, sizeof app.status.note, "%s", app.sh.note);
    ReleaseSRWLockExclusive(&app.sh.lock);
    if (!f)
        return;
    app.viewer->take(*f);
    app.viewer->auto_select();
    // Rates over about a second.
    const double now = now_ms();
    const ViewFrame& cur = app.viewer->current();
    if (app.rate_t == 0) {
        app.rate_t = now;
        app.rate_tick = cur.tick;
        app.rate_instr = cur.instructions;
    } else if (now - app.rate_t >= 1000.0) {
        const double dt = (now - app.rate_t) / 1000.0;
        app.status.tps = (cur.tick - app.rate_tick) / dt;
        app.status.ips = (cur.instructions - app.rate_instr) / dt;
        app.rate_t = now;
        app.rate_tick = cur.tick;
        app.rate_instr = cur.instructions;
    }
    if (app.status.paused || app.status.finished)
        app.status.tps = app.status.ips = 0;
    char title[320];
    int n = snprintf(title, sizeof title, "Nova: %s   tick %llu", app.source, static_cast<unsigned long long>(cur.tick));
    if (app.status.finished || app.status.paused)
        n += snprintf(title + n, sizeof title - n, "   %s", app.status.finished ? "stopped" : "paused");
    else
        n += snprintf(title + n, sizeof title - n, "   %.0f ticks/s   %.0f M instr/s", app.status.tps,
                      app.status.ips / 1e6);
    if (cur.hashed)
        snprintf(title + n, sizeof title - n, "   hash %016llx", static_cast<unsigned long long>(cur.hash));
    SetWindowTextA(app.hwnd, title);
    AcquireSRWLockExclusive(&app.sh.lock);
    app.sh.req = app.viewer->request();
    app.sh.want = true;
    ReleaseSRWLockExclusive(&app.sh.lock);
    WakeConditionVariable(&app.sh.wake);
    if (!app.on_page)
        InvalidateRect(app.hwnd, nullptr, FALSE);
}

void paint(App& app, HDC hdc) {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    const float scale = static_cast<float>(GetDpiForWindow(app.hwnd)) / 96.0f;
    app.canvas.resize(rc.right - rc.left, rc.bottom - rc.top, scale);
    const bool page = app.on_page;
    if (page)
        app.page.frame(app.input, app.canvas);
    else
        app.viewer->frame(app.input, app.canvas, app.status);
    app.canvas.present(hdc);
    // This frame's clicks, keys and wheel are used up.
    app.input.pressed = app.input.released = app.input.dbl = false;
    app.input.wheel = 0;
    app.input.nkeys = app.input.nchars = 0;
    if (page) {
        page_requests(app);
        if (app.on_page) {
            if (!app.run && !app.archive)
                set_title(app);
            if (app.page.wants_frame())
                InvalidateRect(app.hwnd, nullptr, FALSE);
        } else {
            InvalidateRect(app.hwnd, nullptr, FALSE); // switched to the panels
        }
    }
    if (!page || !app.on_page)
        push_commands(app);
    if (!page && app.on_page)
        InvalidateRect(app.hwnd, nullptr, FALSE); // switched to the page
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = g_app;
    if (!app)
        return DefWindowProcW(hwnd, msg, wp, lp);
    UiInput& in = app->input;
    switch (msg) {
    case WM_TIMER:
        if (!app->archive)
            take_frame(*app);
        return 0;
    case kMsgFrame:
        take_frame(*app);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint(*app, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE:
        in.mx = static_cast<short>(LOWORD(lp));
        in.my = static_cast<short>(HIWORD(lp));
        if (!app->tracking) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            app->tracking = true;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSELEAVE:
        app->tracking = false;
        if (!in.down) {
            in.mx = in.my = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        in.mx = static_cast<short>(LOWORD(lp));
        in.my = static_cast<short>(HIWORD(lp));
        in.down = in.pressed = true;
        if (msg == WM_LBUTTONDBLCLK)
            in.dbl = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        in.mx = static_cast<short>(LOWORD(lp));
        in.my = static_cast<short>(HIWORD(lp));
        in.down = false;
        in.released = true;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &p);
        in.mx = p.x;
        in.my = p.y;
        in.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (in.nkeys < 16)
            in.keys[in.nkeys++] = static_cast<u32>(wp);
        if (wp == VK_SPACE && !app->on_page && !app->viewer->editing() && app->run) {
            ViewCommand c = {};
            c.kind = app->status.paused ? kCmdResume : kCmdPause;
            app->viewer->commands().push(c);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CHAR:
        if (wp >= 32 && wp < 127 && in.nchars < 32)
            in.chars[in.nchars++] = static_cast<char>(wp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            const u32 c = app->on_page ? app->page.cursor() : app->viewer->cursor();
            const char* id = c == 1   ? IDC_SIZEWE
                             : c == 2 ? IDC_SIZENS
                             : c == 3 ? IDC_HAND
                             : c == 4 ? IDC_IBEAM
                                      : IDC_ARROW;
            SetCursor(LoadCursorA(nullptr, id));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        end_sim(*app);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int open_window(App& app) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;
    wc.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"NovaView";
    RegisterClassExW(&wc);
    const float scale = static_cast<float>(GetDpiForSystem()) / 96.0f;
    const int w = static_cast<int>(1600 * scale), h = static_cast<int>(980 * scale);
    HWND hwnd = CreateWindowExW(0, L"NovaView", L"Nova", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        fprintf(stderr, "nova: cannot open a window\n");
        return 1;
    }
    app.hwnd = hwnd;
    app.sh.hwnd = hwnd;
    SetWindowTextA(hwnd, app.source);
    if (app.on_page && !app.run && !app.archive)
        set_title(app);
    if (app.run && !begin_sim(app)) {
        fprintf(stderr, "nova: cannot start the simulation thread\n");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// --- headless rendering ---------------------------------------------------------

bool write_png(Canvas& canvas, const char* out, u32 w, u32 h) {
    canvas.flush();
    ByteWriter bytes;
    png_encode(canvas.pixels(), w, h, w, bytes);
    ensure_parent_dir(out);
    if (!write_file(out, bytes.data(), bytes.size())) {
        fprintf(stderr, "nova: cannot write '%s'\n", out);
        return false;
    }
    return true;
}

bool render_png(Viewer& viewer, const RunStatus& st, const char* out, u32 w, u32 h) {
    Canvas canvas;
    if (!canvas.resize(static_cast<i32>(w), static_cast<i32>(h), 1.0f)) {
        fprintf(stderr, "nova: cannot create a %ux%u bitmap\n", w, h);
        return false;
    }
    UiInput none;
    viewer.frame(none, canvas, st);
    return write_png(canvas, out, w, h);
}

bool render_page(SetupPage& page, const char* out, u32 w, u32 h) {
    Canvas canvas;
    if (!canvas.resize(static_cast<i32>(w), static_cast<i32>(h), 1.0f)) {
        fprintf(stderr, "nova: cannot create a %ux%u bitmap\n", w, h);
        return false;
    }
    UiInput none;
    page.frame(none, canvas);
    page.frame(none, canvas); // (the first frame's check, drawn)
    return write_png(canvas, out, w, h);
}

} // namespace

int cmd_view(int argc, char** argv) {
    ViewArgs a;
    if (!parse_view_args(argc, argv, &a)) {
        fprintf(stderr, "usage:\n"
                        "  nova view [--setup <config>] [--set key=value]... [--threads N] [--group NAME]\n"
                        "            [--render out.png [--size WxH]]   (the setup page: the default preset, or that file)\n"
                        "  nova view <config | snapshot> [--threads N] [--set key=value]... [--no-output]\n"
                        "            [--ticks N] [--sample N] [--color NAME] [--render out.png [--size WxH]]\n"
                        "  nova view --run <run directory> [--render out.png [--window K] [--size WxH]]\n"
                        "  (--group NAME: open on the settings page, at that group)\n");
        return 2;
    }
    App* app = new App;
    g_app = app;
    app->viewer = new Viewer;
    app->status.live = a.run_dir == nullptr;
    app->viewer->set_color_by(a.color);
    app->sample = a.sample;
    app->has_threads = a.has_threads;
    app->threads = a.threads;
    app->page.set_front_end(front_end_reader);
    refresh_lists(*app);

    // The setup page: every setting before a run begins (docs/config-plan.md C2).
    if (a.page) {
        char path[MAX_PATH] = {};
        TextBuf err;
        if (a.setup) {
            if (!app->page.load_file(a.setup, &err)) {
                fprintf(stderr, "nova: %s\n", err.c_str());
                return 1;
            }
        } else if (default_preset(path, sizeof path)) {
            if (!app->page.load_file(path, &err)) {
                fprintf(stderr, "nova: %s\n", err.c_str());
                return 1;
            }
            // The presets beside it, for Load (when it isn't in the current folder's configs\).
            snprintf(g_configs, sizeof g_configs, "%s", path);
            char* slash = strrchr(g_configs, '\\');
            if (slash)
                *slash = '\0';
            refresh_lists(*app);
        } else {
            app->page.load_defaults("defaults");
            app->page.set_note("no preset found (configs\\landscape.cfg): every setting at its default", true);
        }
        // --set: changes from the file, made before the page opens.
        for (int i = 0; i < a.set_count; ++i) {
            const Str kv = a.sets[i];
            const usize eq = kv.find('=');
            const usize k = eq < kv.n ? app->page.key_index(kv.sub(0, eq).trim()) : SetupPage::kNoKey;
            TextBuf why;
            if (k == SetupPage::kNoKey || !app->page.set_text(k, kv.sub(eq + 1, kv.n - eq - 1), &why)) {
                fprintf(stderr, "nova: --set '%s': %s\n", a.sets[i], k == SetupPage::kNoKey ? "not a setting" : why.c_str());
                return 1;
            }
        }
        app->on_page = true;
        snprintf(app->source, sizeof app->source, "%s", app->page.source());
        if (a.group && !app->page.select_group_named(a.group)) {
            fprintf(stderr, "nova: --group '%s': no such group (nova keys lists them)\n", a.group);
            return 1;
        }
        if (!a.group)
            app->page.show_first_problem(); // a file with problems opens on the first
        if (a.render) {
            if (!render_page(app->page, a.render, a.width, a.height))
                return 1;
            printf("nova: rendered the setup page %s (%ux%u): %u problem%s, %u settings changed from %s\n", a.render,
                   a.width, a.height, static_cast<unsigned>(app->page.problem_count()),
                   app->page.problem_count() == 1 ? "" : "s", static_cast<unsigned>(app->page.changed_count()),
                   app->page.source());
            return 0;
        }
        return open_window(*app);
    }

    // A finished run's files.
    if (a.run_dir) {
        RunArchive* ar = new RunArchive;
        TextBuf err;
        if (!ar->load(a.run_dir, &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
        app->archive = ar;
        snprintf(app->source, sizeof app->source, "%s", a.run_dir);
        snprintf(app->status.title, sizeof app->status.title, "%s", a.run_dir);
        app->status.windows = ar->windows();
        app->status.window = ar->windows() ? ar->windows() - 1 : 0;
        if (a.has_window && a.window < ar->windows())
            app->status.window = static_cast<u32>(a.window);
        for (int pass = 0; pass < 2; ++pass) {
            ar->build(app->status.window, app->viewer->request(), &app->archive_frame);
            app->viewer->take(app->archive_frame);
            app->viewer->auto_select();
        }
        // Its settings, from its replay log's config lines, read-only.
        TextBuf perr;
        app->page.load_text(ar->config_text(), a.run_dir, &perr);
        app->page.set_mode(kSetupArchive, a.run_dir);
        char info[240];
        char log_path[MAX_PATH];
        snprintf(log_path, sizeof log_path, "%s\\replay.log", a.run_dir);
        ReplayLog log;
        TextBuf lerr;
        if (ar->config_text().n == 0)
            snprintf(info, sizeof info, "no replay.log in this folder: the run's settings aren't known");
        else if (log.load_file(log_path, &lerr) && log.knob_count())
            snprintf(info, sizeof info,
                     "from its replay log; it also changed %llu setting%s mid-run (knob records from before settings "
                     "were fixed), which replay applies",
                     static_cast<unsigned long long>(log.knob_count()), log.knob_count() == 1 ? "" : "s");
        else
            snprintf(info, sizeof info, "from its replay log: what it started with");
        app->page.set_info(info);
        if (a.group) { // open on its settings, at that group
            if (!app->page.select_group_named(a.group)) {
                fprintf(stderr, "nova: --group '%s': no such group (nova keys lists them)\n", a.group);
                return 1;
            }
            app->on_page = true;
        }
        if (a.render && app->on_page) {
            if (!render_page(app->page, a.render, a.width, a.height))
                return 1;
            printf("nova: rendered %s's settings %s (%ux%u)\n", a.run_dir, a.render, a.width, a.height);
            return 0;
        }
        if (a.render) {
            if (!render_png(*app->viewer, app->status, a.render, a.width, a.height))
                return 1;
            printf("nova: rendered %s (%ux%u), window %u of %u\n", a.render, a.width, a.height,
                   app->status.window + 1, app->status.windows);
            return 0;
        }
        return open_window(*app);
    }

    // A live run: from a config, or continuing a snapshot.
    TextBuf err;
    if (is_snapshot(a.source)) {
        Run* run = new Run;
        RunKeys* keys = new RunKeys;
        app->run = run;
        app->keys = keys;
        RunKeys defaults; // a snapshot: the machine defaults (all threads but one, below-normal priority)
        const u32 threads = machine_threads(defaults, a.has_threads ? a.threads : 0);
        apply_priority(defaults);
        Array<u8> bytes;
        if (!read_file(a.source, &bytes)) {
            fprintf(stderr, "nova: cannot read '%s'\n", a.source);
            return 1;
        }
        if (!run->init_from_snapshot(bytes.data(), bytes.size(), threads, &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
        run->lock_knobs(); // its settings are the snapshot's, fixed (docs/config-plan.md C3)
        app->outputs = false; // a snapshot continues with its outputs off
        app->tick_limit = a.has_ticks ? a.ticks : 0;
        run->keep_genebank(true);
        app->session = new ViewSession;
        app->session->attach(run, a.sample);
        page_from_run(*app, kSetupSnapshot, a.source);
        app->page.set_info("the snapshot's settings: a continued snapshot keeps them");
    } else {
        if (!start_live(*app, a.source, a.sets, a.set_count, !a.no_output, a.has_ticks, a.ticks, &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
        page_from_run(*app, kSetupLive, a.source);
        app->page.set_info("what this run started with; fixed for the whole run");
    }
    Run* run = app->run;
    snprintf(app->source, sizeof app->source, "%s", a.source);
    snprintf(app->status.title, sizeof app->status.title, "%s", a.source);
    if (a.group) { // open on its settings, at that group
        if (!app->page.select_group_named(a.group)) {
            fprintf(stderr, "nova: --group '%s': no such group (nova keys lists them)\n", a.group);
            return 1;
        }
        app->on_page = true;
    }

    if (a.render) {
        const u64 ticks = app->tick_limit;
        const double t0 = now_ms();
        for (u64 i = 0; i < ticks; ++i) {
            run->step();
            app->session->after_step();
        }
        const double secs = (now_ms() - t0) / 1000.0;
        app->session->flush_sample();
        ViewFrame f;
        for (int pass = 0; pass < 3; ++pass) {
            ViewRequest q = app->viewer->request();
            q.hash = true;
            app->session->build(q, &f);
            app->viewer->take(f);
            app->viewer->auto_select();
        }
        app->status.paused = true;
        app->status.tps = secs > 0 ? ticks / secs : 0;
        app->status.ips = secs > 0 ? run->instructions() / secs : 0;
        const bool ok = app->on_page ? render_page(app->page, a.render, a.width, a.height)
                                     : render_png(*app->viewer, app->status, a.render, a.width, a.height);
        finish_run(*app, "view_render", secs);
        if (!ok)
            return 1;
        printf("nova: rendered %s (%ux%u) at tick %llu, hash %016llx\n", a.render, a.width, a.height,
               static_cast<unsigned long long>(run->world().tick()),
               static_cast<unsigned long long>(run->world().hash()));
        return 0;
    }
    return open_window(*app);
}

} // namespace nova
