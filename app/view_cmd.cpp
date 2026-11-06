// nova view -- the analysis UI's Win32 shell (Phase 6, design.md 6.12).
//
//   nova view <config | snapshot> [--threads N] [--set key=value]... [--no-output]
//             [--ticks N] [--sample N] [--color NAME] [--render out.png [--size WxH]]
//   nova view --run <run directory> [--render out.png [--window K] [--size WxH]]
//
// A config runs live, writing the outputs it names (a watched run is a real
// run; --no-output turns them all off). A snapshot continues live from its
// tick with its outputs off. --run browses a finished run's files without
// simulating. --render draws one frame into a PNG instead of opening a window
// (after --ticks N ticks of a live run).
//
// Threads (plan C2): a sim thread owns the Run and steps it; the UI thread
// owns the window. After a step the sim thread builds a view frame when the
// UI has asked for one, between ticks, and carries on. Run controls and knob
// changes go from the UI into a queue that the sim thread applies between
// ticks (knobs via Run::schedule_knob at the current tick), so they are
// logged and replay exactly: watching changes nothing.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "app.h"
#include "canvas.h"
#include "nova/png.h"
#include "nova/snapshot.h"
#include "nova/view.h"
#include "viewer.h"

namespace nova {

namespace {

struct ViewArgs {
    const char* source = nullptr;  // a config or a snapshot
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
    return (a->source != nullptr) != (a->run_dir != nullptr);
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

// Text for a ViewCommand's knob, as "key = value".
void note(char* out, usize n, const char* fmt_a, const char* b, const char* c) {
    snprintf(out, n, fmt_a, b, c);
}

// --- the live run's shared state ------------------------------------------------

const UINT kMsgFrame = WM_APP + 1;

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    CONDITION_VARIABLE wake = CONDITION_VARIABLE_INIT;
    Array<ViewCommand> cmds;
    ViewRequest req;
    bool want = true;               // the UI wants a frame
    bool quit = false;
    ViewFrame* ready = nullptr;     // a built frame the UI hasn't taken yet
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
    u64 tick_limit = 0;   // pause after this many ticks (0 = none)
    double seconds = 0;   // ... or this much wall time
    i64 after = -1;       // ... or this many ticks after the first replicator
    ViewSession session;
    ViewFrame frames[2];
    Shared sh;
    HANDLE thread = nullptr;
    // a finished run
    RunArchive* archive = nullptr;
    ViewFrame archive_frame;
    // the UI
    Viewer viewer;
    Canvas canvas;
    UiInput input;
    RunStatus status;
    HWND hwnd = nullptr;
    double rate_t = 0;
    u64 rate_tick = 0, rate_instr = 0;
    char source[160] = {};
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
            case kCmdKnob: {
                if (finished)
                    break;
                TextBuf err;
                if (run.schedule_knob(run.world().tick(), c.key, c.text, &err)) {
                    snprintf(msg, sizeof msg, "%s = %s from tick %llu", c.key, c.text,
                             static_cast<unsigned long long>(run.world().tick()));
                    set_note(sh, msg);
                } else {
                    set_note(sh, err.c_str());
                }
                break;
            }
            default:
                break;
            }
        }

        bool ran = false;
        double now = now_ms();
        if (!finished && (!paused || step_once) && (speed <= 0 || now >= next_due || step_once)) {
            run.step();
            app.session.after_step();
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
                app.session.flush_sample();
            req.hash = still;
            last_req = req;
            ViewFrame* f = &app.frames[next];
            next ^= 1;
            app.session.build(req, f);
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

// --- the window -------------------------------------------------------------------

void push_commands(App& app) {
    Array<ViewCommand>& cmds = app.viewer.commands();
    if (app.archive) {
        for (usize i = 0; i < cmds.size(); ++i) {
            if (cmds[i].kind != kCmdWindow)
                continue;
            const u32 n = app.archive->windows();
            const u32 w = static_cast<u32>(cmds[i].value);
            if (w < n && w != app.status.window) {
                app.status.window = w;
                app.archive->build(w, app.viewer.request(), &app.archive_frame);
                app.viewer.take(app.archive_frame);
                InvalidateRect(app.hwnd, nullptr, FALSE);
            }
        }
        cmds.clear();
        return;
    }
    AcquireSRWLockExclusive(&app.sh.lock);
    for (usize i = 0; i < cmds.size(); ++i)
        app.sh.cmds.push(cmds[i]);
    app.sh.req = app.viewer.request();
    ReleaseSRWLockExclusive(&app.sh.lock);
    WakeConditionVariable(&app.sh.wake);
    cmds.clear();
}

void take_frame(App& app) {
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
    app.viewer.take(*f);
    app.viewer.auto_select();
    // Rates over about a second.
    const double now = now_ms();
    const ViewFrame& cur = app.viewer.current();
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
    app.sh.req = app.viewer.request();
    app.sh.want = true;
    ReleaseSRWLockExclusive(&app.sh.lock);
    WakeConditionVariable(&app.sh.wake);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void paint(App& app, HDC hdc) {
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    const float scale = static_cast<float>(GetDpiForWindow(app.hwnd)) / 96.0f;
    app.canvas.resize(rc.right - rc.left, rc.bottom - rc.top, scale);
    app.viewer.frame(app.input, app.canvas, app.status);
    app.canvas.present(hdc);
    // This frame's clicks, keys and wheel are used up.
    app.input.pressed = app.input.released = app.input.dbl = false;
    app.input.wheel = 0;
    app.input.nkeys = app.input.nchars = 0;
    push_commands(app);
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
        if (wp == VK_SPACE && !app->viewer.knob_editing() && !app->archive) {
            ViewCommand c = {};
            c.kind = app->status.paused ? kCmdResume : kCmdPause;
            app->viewer.commands().push(c);
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
            const u32 c = app->viewer.cursor();
            SetCursor(LoadCursorA(nullptr, c == 1 ? IDC_SIZEWE : (c == 2 ? IDC_SIZENS : (c == 3 ? IDC_HAND : IDC_ARROW))));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (app->thread) {
            AcquireSRWLockExclusive(&app->sh.lock);
            app->sh.quit = true;
            ReleaseSRWLockExclusive(&app->sh.lock);
            WakeConditionVariable(&app->sh.wake);
            WaitForSingleObject(app->thread, INFINITE);
            CloseHandle(app->thread);
            app->thread = nullptr;
        }
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
    if (app.run) {
        app.thread = CreateThread(nullptr, 0, sim_main, &app, 0, nullptr);
        if (!app.thread) {
            fprintf(stderr, "nova: cannot start the simulation thread\n");
            return 1;
        }
        SetTimer(hwnd, 1, 100, nullptr); // a fallback poll; frames arrive by message
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

bool render_png(Viewer& viewer, const RunStatus& st, const char* out, u32 w, u32 h) {
    Canvas canvas;
    if (!canvas.resize(static_cast<i32>(w), static_cast<i32>(h), 1.0f)) {
        fprintf(stderr, "nova: cannot create a %ux%u bitmap\n", w, h);
        return false;
    }
    UiInput none;
    viewer.frame(none, canvas, st);
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

} // namespace

int cmd_view(int argc, char** argv) {
    ViewArgs a;
    if (!parse_view_args(argc, argv, &a)) {
        fprintf(stderr, "usage:\n"
                        "  nova view <config | snapshot> [--threads N] [--set key=value]... [--no-output]\n"
                        "            [--ticks N] [--sample N] [--color NAME] [--render out.png [--size WxH]]\n"
                        "  nova view --run <run directory> [--render out.png [--window K] [--size WxH]]\n");
        return 2;
    }
    App* app = new App;
    g_app = app;
    app->status.live = a.run_dir == nullptr;
    app->viewer.set_color_by(a.color);

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
            ar->build(app->status.window, app->viewer.request(), &app->archive_frame);
            app->viewer.take(app->archive_frame);
            app->viewer.auto_select();
        }
        if (a.render) {
            if (!render_png(app->viewer, app->status, a.render, a.width, a.height))
                return 1;
            printf("nova: rendered %s (%ux%u), window %u of %u\n", a.render, a.width, a.height,
                   app->status.window + 1, app->status.windows);
            return 0;
        }
        return open_window(*app);
    }

    // A live run: from a config, or continuing a snapshot.
    Run* run = new Run;
    RunKeys* keys = new RunKeys;
    app->run = run;
    app->keys = keys;
    TextBuf err;
    RunKeys defaults; // a snapshot: the machine defaults (all threads but one, below-normal priority)
    const u32 threads = machine_threads(defaults, a.has_threads ? a.threads : 0);
    if (is_snapshot(a.source)) {
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
        app->outputs = false; // a snapshot continues with its outputs off
        app->tick_limit = a.has_ticks ? a.ticks : 0;
    } else {
        Config& cfg = run->config();
        Config::ParseError pe;
        if (!cfg.load_file(a.source, &pe)) {
            fprintf(stderr, "nova: %s: %s\n", a.source, pe.msg);
            return 1;
        }
        for (int i = 0; i < a.set_count; ++i)
            if (!apply_set(cfg, a.sets[i]))
                return 1;
        read_run_keys(cfg, keys);
        if (a.has_threads)
            keys->threads = a.threads;
        apply_priority(*keys);
        if (!run->init(machine_threads(*keys, keys->threads), &err)) {
            fprintf(stderr, "nova: %s\n", err.c_str());
            return 1;
        }
        print_budget_notes(*run);
        Str unused;
        if (cfg.first_unused(&unused)) {
            fprintf(stderr, "nova: config key '%.*s' is not used by anything (typo?)\n", static_cast<int>(unused.n),
                    unused.p);
            return 1;
        }
        app->outputs = !a.no_output;
        if (app->outputs && !open_outputs(*run, *keys))
            return 1;
        app->tick_limit = a.has_ticks ? a.ticks : keys->ticks;
        app->seconds = keys->seconds;
        app->after = keys->after;
    }
    run->keep_genebank(true);
    app->session.attach(run, a.sample);
    snprintf(app->source, sizeof app->source, "%s", a.source);
    snprintf(app->status.title, sizeof app->status.title, "%s", a.source);

    if (a.render) {
        const u64 ticks = app->tick_limit;
        const double t0 = now_ms();
        for (u64 i = 0; i < ticks; ++i) {
            run->step();
            app->session.after_step();
        }
        const double secs = (now_ms() - t0) / 1000.0;
        app->session.flush_sample();
        ViewFrame f;
        for (int pass = 0; pass < 3; ++pass) {
            ViewRequest q = app->viewer.request();
            q.hash = true;
            app->session.build(q, &f);
            app->viewer.take(f);
            app->viewer.auto_select();
        }
        app->status.paused = true;
        app->status.tps = secs > 0 ? ticks / secs : 0;
        app->status.ips = secs > 0 ? run->instructions() / secs : 0;
        const bool ok = render_png(app->viewer, app->status, a.render, a.width, a.height);
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
