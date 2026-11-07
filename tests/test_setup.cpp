// The setup page (docs/config-plan.md C2-C5), in suite viewer: driven
// headless by made-up input, like the panels.
//
//   - a checkbox toggles; a slider drags and clamps (linear and logarithmic);
//     a drop-down picks; a typed value applies (held to its range);
//   - search and "changed only" find the right rows; reset goes back to the
//     file; a needs switch greys its settings (which keep their values);
//   - the check: problems by setting, several at once, Start blocked while
//     one is open; computed defaults worked out; the world's size; a
//     landscape's summary;
//   - Save then Load gives the same settings back;
//   - Start's run.cfg holds every effective setting, and a run started from
//     it hashes exactly as `nova run` on the file, as `nova view` on it (the
//     window's own start) and as the settings it was made from;
//   - the page renders headless, editable and read-only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "check.h"
#include "helpers.h"
#include "nova/budget.h"
#include "nova/check.h"
#include "nova/io.h"
#include "nova/keys.h"
#include "nova/landscape.h"
#include "setup.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

// What `nova run` reads before the world (app/main.cpp read_run_keys).
void run_keys_here(Config& cfg) {
    (void)key_u64(cfg, "run.ticks");
    (void)key_u64(cfg, "run.threads");
    (void)key_f64(cfg, "run.seconds");
    (void)key_i64(cfg, "run.after_replicator");
    (void)key_u64(cfg, "stats.window");
    (void)key_u64(cfg, "replay.checkpoint_every");
    (void)key_u64(cfg, "output.snapshot_every");
    (void)key_u64(cfg, "stats.nodes_every");
    (void)key_str(cfg, "budget.threads");
    (void)key_choice(cfg, "budget.priority");
    const char* const outputs[] = {"csv", "nodes", "signals", "replay", "snapshot", "genebank", "lineages", "result"};
    for (const char* o : outputs) {
        char name[32];
        snprintf(name, sizeof name, "output.%s", o);
        (void)key_str(cfg, name);
    }
}

u32 group_index(const char* name) {
    for (u32 g = 0; g < kKeyGroupCount; ++g)
        if (strcmp(kKeyGroups[g], name) == 0)
            return g;
    return 0;
}

bool ci_has(Str hay, const char* w) {
    const usize n = strlen(w);
    for (usize i = 0; i + n <= hay.n; ++i) {
        usize k = 0;
        while (k < n) {
            char a = hay.p[i + k], b = w[k];
            a = a >= 'A' && a <= 'Z' ? static_cast<char>(a + 32) : a;
            b = b >= 'A' && b <= 'Z' ? static_cast<char>(b + 32) : b;
            if (a != b)
                break;
            ++k;
        }
        if (k == n)
            return true;
    }
    return false;
}

bool key_mentions(const Key& k, const char* w) { return ci_has(k.name, w) || ci_has(k.label, w) || ci_has(k.help, w); }

bool same(const Key& k, Str a, Str b) {
    a = a.trim();
    b = b.trim();
    if (a.eq(b))
        return true;
    double x = 0, y = 0;
    if ((k.kind == KeyKind::number || k.kind == KeyKind::whole) && parse_f64(a, &x) && parse_f64(b, &y))
        return x == y;
    bool p = false, q = false;
    return k.kind == KeyKind::flag && parse_bool(a, &p) && parse_bool(b, &q) && p == q;
}

struct PageRig {
    SetupPage page;
    Canvas canvas;

    bool start(const char* cfg) {
        if (!canvas.resize(1600, 1000, 1.0f))
            return false;
        page.set_machine_check(false);
        page.set_front_end(run_keys_here);
        TextBuf err;
        const bool ok = page.load_text(cfg, "test.cfg", &err);
        if (!ok)
            fprintf(stderr, "  load_text: %s\n", err.c_str());
        draw();
        return ok;
    }
    void draw(const UiInput& in) { page.frame(in, canvas); }
    void draw() {
        UiInput none;
        draw(none);
    }
    void click(i32 x, i32 y) {
        UiInput in;
        in.mx = x;
        in.my = y;
        in.pressed = in.released = true;
        draw(in);
        draw();
    }
    void type(const char* s, u32 backspaces, bool enter) {
        UiInput in;
        for (u32 i = 0; i < backspaces && in.nkeys < 15; ++i)
            in.keys[in.nkeys++] = 0x08;
        draw(in);
        UiInput t;
        for (const char* p = s; *p && t.nchars < 32; ++p)
            t.chars[t.nchars++] = *p;
        if (enter)
            t.keys[t.nkeys++] = 0x0D;
        draw(t);
        draw();
    }
    usize key(const char* name) const { return page.key_index(name); }
    void show(const char* group) {
        page.select_group(group_index(group));
        draw();
    }
    Rect ctl(const char* name) const {
        Rect r;
        if (!page.control_rect(key(name), &r))
            fprintf(stderr, "  %s: not on screen\n", name);
        return r;
    }
    bool is(const char* name, const char* v) {
        const usize k = key(name);
        const bool ok = page.shown(k).eq(v);
        if (!ok)
            fprintf(stderr, "  %s shows '%.*s', expected '%s'\n", name, static_cast<int>(page.shown(k).n),
                    page.shown(k).p, v);
        return ok;
    }
    double num(const char* name) {
        const Str s = page.shown(key(name));
        char b[64];
        snprintf(b, sizeof b, "%.*s", static_cast<int>(s.n), s.p);
        return atof(b);
    }
    bool set(const char* name, const char* v) {
        TextBuf why;
        const bool ok = page.set_text(key(name), v, &why);
        if (!ok)
            fprintf(stderr, "  set %s = %s: %s\n", name, v, why.c_str());
        draw();
        return ok;
    }
    const SettingProblem* problem(const char* name) {
        const SettingsCheck& ch = page.check();
        for (usize i = 0; i < ch.problems.size(); ++i)
            if (strcmp(ch.problems[i].key, name) == 0)
                return &ch.problems[i];
        return nullptr;
    }
};

// A small soup world and its run keys.
const char* const kWorld = "seed = 7\n"
                           "world.model = soup\n"
                           "world.nodes = 6\n"
                           "world.soup_size = 2048\n"
                           "world.millicores = 1000\n"
                           "spark.initial = 20\n"
                           "spark.rate = 0.25\n"
                           "run.ticks = 50\n";

// --- the widgets ------------------------------------------------------------------------

void test_page_widgets() {
    PageRig* r = new PageRig;
    CHECK(r->start(kWorld));
    CHECK(r->page.problem_count() == 0);
    CHECK(r->page.changed_count() == 0);

    // A checkbox toggles; its needs switch greys what depends on it, values kept.
    r->show("Heat");
    CHECK(r->is("heat.enabled", "true"));
    CHECK(!r->page.greyed(r->key("heat.cool")));
    Rect c = r->ctl("heat.enabled");
    r->click(c.x + 8, c.y + c.h / 2);
    CHECK(r->is("heat.enabled", "false"));
    CHECK(r->page.changed(r->key("heat.enabled")));
    CHECK(r->page.greyed(r->key("heat.cool")) && r->page.greyed(r->key("heat.cpu.warm")));
    // A greyed setting takes no input: a click on its number field opens nothing.
    const Rect hc = r->ctl("heat.cool");
    r->click(hc.right() - 40, hc.y + hc.h / 2);
    CHECK(!r->page.editing());
    CHECK(r->is("heat.cool", "0.1")); // its value is kept
    r->click(c.x + 8, c.y + c.h / 2);
    CHECK(r->is("heat.enabled", "true") && !r->page.greyed(r->key("heat.cool")));
    CHECK(!r->page.changed(r->key("heat.enabled"))); // as the file has it (the default)

    // A slider drags, and clamps to its range: isa.stack_depth is 1 to 16.
    r->show("CPU and cores");
    c = r->ctl("isa.stack_depth");
    const i32 track_x = c.x + 6, track_w = c.w - 110 - 18;
    UiInput press;
    press.mx = track_x + track_w / 2;
    press.my = c.y + c.h / 2;
    press.pressed = press.down = true;
    r->draw(press);
    UiInput move;
    move.mx = track_x + track_w + 300; // far past the right end
    move.my = c.y + 200;               // and off the row
    move.down = true;
    r->draw(move);
    CHECK(r->is("isa.stack_depth", "8")); // nothing applies until it is let go
    UiInput up;
    up.mx = move.mx;
    up.my = move.my;
    up.released = true;
    r->draw(up);
    r->draw();
    CHECK(r->is("isa.stack_depth", "16"));
    r->click(track_x - 3, c.y + c.h / 2); // the left end
    CHECK(r->is("isa.stack_depth", "1"));
    r->click(track_x + track_w / 2, c.y + c.h / 2); // the middle of 1..16
    const double mid = r->num("isa.stack_depth");
    CHECK(mid >= 8 && mid <= 9);
    // A logarithmic one: hazard.copy (0 to 1, default 5e-4). Its left end is 0 (off).
    r->show("Hazards");
    c = r->ctl("hazard.copy");
    const i32 hx = c.x + 6, hw = c.w - 110 - 18;
    r->click(hx, c.y + c.h / 2);
    CHECK(r->is("hazard.copy", "0"));
    r->click(hx + hw, c.y + c.h / 2);
    CHECK(r->is("hazard.copy", "1"));
    r->click(hx + hw / 2, c.y + c.h / 2);
    const double h_mid = r->num("hazard.copy");
    CHECK(h_mid > 1e-6 && h_mid < 1e-3); // log: the middle is decades below the top
    r->click(hx + hw * 3 / 4, c.y + c.h / 2);
    CHECK(r->num("hazard.copy") > h_mid * 10);

    // A drop-down picks: world.graph, from ring to procedural.
    r->show("World and land");
    CHECK(r->is("world.graph", "ring"));
    CHECK(r->page.greyed(r->key("world.links")) && !r->page.greyed(r->key("world.chords")));
    c = r->ctl("world.graph");
    r->click(c.x + 20, c.y + c.h / 2); // opens it
    const i32 ih = r->canvas.line_h(kFontUi) + r->canvas.px(6);
    r->click(c.x + 20, c.y + c.h + 2 + ih + ih / 2); // its second option
    CHECK(r->is("world.graph", "procedural"));
    CHECK(!r->page.greyed(r->key("world.links")) && r->page.greyed(r->key("world.chords")));

    // A typed value applies: world.nodes 6 -> 12; out of range, held to it; nonsense refused.
    c = r->ctl("world.nodes");
    r->click(c.right() - 40, c.y + c.h / 2);
    CHECK(r->page.editing());
    r->type("12", 8, true);
    CHECK(!r->page.editing() && r->is("world.nodes", "12"));
    r->click(c.right() - 40, c.y + c.h / 2);
    r->type("2000000", 8, true);
    CHECK(r->is("world.nodes", "1000000")); // the most it can be
    r->click(c.right() - 40, c.y + c.h / 2);
    r->type("lots", 10, true);
    CHECK(r->page.editing() && r->is("world.nodes", "1000000")); // still editing, nothing applied
    UiInput esc;
    esc.keys[0] = 0x1B;
    esc.nkeys = 1;
    r->draw(esc);
    CHECK(!r->page.editing());
    r->click(c.right() - 40, c.y + c.h / 2);
    r->type("12", 8, false);
    r->click(5, 990); // a click elsewhere applies it too
    CHECK(!r->page.editing() && r->is("world.nodes", "12"));
    // A text field: seed.genome, typed.
    r->show("Sparks");
    c = r->ctl("seed.genome");
    CHECK(r->page.greyed(r->key("seed.node"))); // needs a genome
    r->click(c.x + 30, c.y + c.h / 2);
    r->type("SENSE ALLOC", 0, true);
    CHECK(r->is("seed.genome", "SENSE ALLOC"));
    CHECK(!r->page.greyed(r->key("seed.node")));

    // Changed only: exactly what differs from the file.
    r->page.set_changed_only(true);
    r->draw();
    usize changed_rows = 0;
    bool only_changed = true;
    for (usize i = 0; i < r->page.row_count(); ++i) {
        const usize k = r->page.row_key(i);
        if (k == SetupPage::kNoKey)
            continue;
        ++changed_rows;
        only_changed = only_changed && r->page.changed(k);
    }
    CHECK(only_changed && changed_rows == r->page.changed_count());
    CHECK(changed_rows == 5); // isa.stack_depth, hazard.copy, world.graph, world.nodes, seed.genome
    // Reset: world.nodes back to the file's 6, and off the list.
    Rect row;
    CHECK(r->page.row_rect(r->key("world.nodes"), &row));
    const Rect wc = r->ctl("world.nodes");
    const i32 reset_x = wc.x + wc.w + 12 + 150 + 2 + 14 + 25;
    r->click(reset_x, row.y + 15);
    CHECK(r->is("world.nodes", "6") && !r->page.changed(r->key("world.nodes")));
    r->draw();
    bool gone = true;
    for (usize i = 0; i < r->page.row_count(); ++i)
        gone = gone && r->page.row_key(i) != r->key("world.nodes");
    CHECK(gone && r->page.changed_count() == 4);
    r->page.set_changed_only(false);

    // Search: names, labels and help, every word.
    r->page.set_search("disk rot");
    r->draw();
    bool has_disk_rot = false, none_hidden = true, headings = false;
    usize hits = 0;
    for (usize i = 0; i < r->page.row_count(); ++i) {
        const usize k = r->page.row_key(i);
        if (k == SetupPage::kNoKey) {
            headings = true;
            continue;
        }
        ++hits;
        has_disk_rot = has_disk_rot || k == r->key("disk.rot");
        none_hidden = none_hidden && !(key_at(k).flags & kKeyHidden);
    }
    usize want = 0;
    for (usize k = 0; k < key_count(); ++k)
        want += !(key_at(k).flags & kKeyHidden) && key_mentions(key_at(k), "disk") && key_mentions(key_at(k), "rot");
    CHECK(has_disk_rot && none_hidden && headings && hits == want && hits >= 1);
    // Typed into the search box (the list's top left), it filters as it goes.
    r->page.set_search("");
    r->show("Run and output");
    Canvas& cv = r->canvas;
    const i32 sx = cv.px(4) + cv.px(220) + cv.px(4) + cv.px(6) + 40;
    const i32 sy = cv.px(70) + cv.px(4) + cv.line_h(kFontBold) + cv.px(6) + cv.px(6) + 10;
    r->click(sx, sy);
    CHECK(r->page.editing());
    r->type("bandwidth", 0, false);
    usize bw = 0;
    bool bw_ok = true;
    for (usize i = 0; i < r->page.row_count(); ++i) {
        const usize k = r->page.row_key(i);
        if (k == SetupPage::kNoKey)
            continue;
        ++bw;
        bw_ok = bw_ok && key_mentions(key_at(k), "bandwidth");
    }
    CHECK(bw_ok && bw >= 2); // edge.bandwidth, bridge.bandwidth
    UiInput esc2;
    esc2.keys[0] = 0x1B;
    esc2.nkeys = 1;
    r->draw(esc2); // Esc clears the search
    r->draw();
    CHECK(!r->page.editing() && r->page.row_count() > 0 && r->page.row_key(0) == r->key("run.ticks"));
    delete r;
}

// --- the check ----------------------------------------------------------------------------

void test_page_check() {
    PageRig* r = new PageRig;
    CHECK(r->start(kWorld));
    // Computed defaults worked out: chords are half the nodes; spark.max_len 128 at 2048 codons.
    CHECK(r->is("world.chords", "3") && r->is("spark.max_len", "128"));
    CHECK(r->set("world.nodes", "40"));
    CHECK(r->is("world.chords", "20"));
    // Two cross-check problems at once, each by its setting; Start stays off.
    r->page.set_folder("nova_test_setup_block");
    CHECK(r->page.can_start());
    CHECK(r->set("spark.min_len", "100") && r->set("spark.max_len", "50"));
    CHECK(r->set("cost.table", "classic") && r->set("cpu.e_share", "0.5"));
    const SettingProblem* p1 = r->problem("spark.min_len");
    const SettingProblem* p2 = r->problem("cpu.e_share");
    CHECK(p1 && strstr(p1->what, "must not exceed spark.max_len"));
    CHECK(p2 && strstr(p2->what, "needs a timed cost table"));
    CHECK(r->page.problem_count() == 2 && !r->page.can_start());
    // The problem shows next to its setting: the row grows a line for it.
    r->show("Sparks");
    Rect row;
    CHECK(r->page.row_rect(r->key("spark.min_len"), &row) && row.h > 30);
    CHECK(r->page.row_rect(r->key("spark.rate"), &row) && row.h == 30);
    // Start is a dead button while a problem is open.
    r->page.requests().clear();
    const Rect sw = r->page.start_button(false);
    CHECK(sw.w > 0);
    r->click(sw.x + sw.w / 2, sw.y + sw.h / 2);
    CHECK(r->page.requests().size() == 0);
    // Fixed (the defaults again): Start asks the shell to start, with the folder.
    r->page.unset(r->key("spark.min_len"));
    r->page.unset(r->key("cpu.e_share"));
    r->draw();
    CHECK(r->page.problem_count() == 0 && r->page.can_start());
    r->click(sw.x + sw.w / 2, sw.y + sw.h / 2);
    CHECK(r->page.requests().size() == 1 && r->page.requests()[0].kind == kReqStartWindow &&
          strcmp(r->page.requests()[0].folder, "nova_test_setup_block") == 0);
    r->page.requests().clear();
    // A range problem in the loaded file, and a key that isn't a setting.
    TextBuf err;
    CHECK(r->page.load_text("world.model = soup\nworld.nodes = 0\nworld.ndoes = 5\n", "bad.cfg", &err));
    r->draw();
    const SettingProblem* p3 = r->problem("world.nodes");
    CHECK(p3 && strstr(p3->what, "must be between 1 and 1000000"));
    CHECK(r->problem("") && strstr(r->problem("")->what, "world.ndoes"));
    CHECK(!r->page.can_start());
    // The world's size against the machine (the budgets' sums).
    CHECK(r->page.load_text(kWorld, "test.cfg", &err));
    r->page.set_machine_check(true);
    const SettingsCheck& ch = r->page.check();
    CHECK(ch.soup && ch.nodes == 6 && ch.soup_size == 2048 && ch.ram == 6.0 * 2048.0 * 20.0 && ch.ram_avail > 0);
    r->page.set_machine_check(false);
    // A landscape: nova land's summary.
    CHECK(r->page.load_text("seed = 3\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 150\n"
                            "land.scale = 5\nland.fraction = 0.3\nland.resolution = 96\nworld.soup_size = 1024\n",
                            "land.cfg", &err));
    const SettingsCheck& lc = r->page.check();
    u32 sum = 0;
    for (u32 d = 0; d < 14; ++d)
        sum += lc.land.hist[d];
    CHECK(lc.landscape && lc.land.built && lc.land.nodes == 150 && lc.land.peopled >= 1 && sum == 150 &&
          lc.land.largest[0] >= 1 && r->page.problem_count() == 0);
    // ... and one that can't be built: more nodes than the land holds.
    CHECK(r->set("land.fraction", "0.01") && r->set("world.nodes", "100000"));
    CHECK(r->problem("world.nodes") != nullptr && !r->page.check().land.built);
    r->draw(); // (draws with the problem)
    delete r;
}

// The distinct substrates over a built world's nodes, as nova land counts them.
u32 world_substrates(const World& w) {
    Array<u64> subs;
    for (u32 n = 0; n < w.node_count(); ++n) {
        u64 key = 0;
        for (u32 t = 0; t < kSubstrateTypes; ++t)
            key = key << 8 | w.substrate_level(n, t);
        subs.push(key);
    }
    sort_u64(subs.data(), subs.size());
    u32 distinct = 0;
    for (usize i = 0; i < subs.size(); ++i)
        distinct += i == 0 || subs[i] != subs[i - 1];
    return distinct;
}

// What the merged builds add to the check (docs/results/integration.md): the
// archive in the world's size, with the fit check's own sums (world_footprint,
// which resolve_budgets uses too), and the landscape summary's substrates.
void test_page_check_merged() {
    // The archive (disk tiers, W4) on top of the fast disk, in RAM.
    TextBuf t;
    t.append(kWorld);
    t.append("world.disk = 1000\nworld.archive = 500\n");
    SettingsCheck dc;
    check_settings(t.view(), &dc, run_keys_here, false);
    const double tier = 6.0 * 1500.0 * disk_bytes_per_codon(false);
    CHECK(dc.problems.size() == 0 && dc.disk == 1000 && dc.archive == 500 && dc.ram == 6.0 * 2048.0 * 20.0 + tier);
    Config c;
    Config::ParseError pe;
    CHECK(c.parse(t.view(), &pe));
    WorldFootprint fp;
    world_footprint(c, &fp);
    CHECK(fp.ram == dc.ram && fp.archive == 500 && fp.disk_file == 0);
    // ... and by default a quarter of the disk (the engine's defaults: the test
    // runner's legacy ones have no archive).
    CHECK(Config::set_fallback(nullptr, nullptr));
    t.clear();
    t.append(kWorld);
    t.append("world.disk = 1000\naccel.gpu = cpu\naccel.npu = cpu\nwarp.device = cpu\n");
    SettingsCheck dq;
    check_settings(t.view(), &dq, run_keys_here, false);
    CHECK(dq.archive == 250 && dq.ram == 6.0 * 2048.0 * 20.0 + 6.0 * 1250.0 * disk_bytes_per_codon(false));
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    // The landscape summary counts the substrates as nova land does, and the
    // cache follows the substrates' own settings.
    const char* land = "seed = 3\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 150\nland.scale = 5\n"
                       "land.fraction = 0.3\nland.resolution = 96\nworld.soup_size = 1024\nsubstrate.enabled = true\n";
    SettingsCheck lc;
    check_settings(land, &lc, run_keys_here, false);
    Run a;
    CHECK(make_run(a, land, 1));
    const u32 want = world_substrates(a.world());
    CHECK(lc.land.built && lc.land.substrates && want > 1 && lc.land.substrates_distinct == want);
    t.clear();
    t.append(land);
    t.append("substrate.smooth = 0.3\n");
    check_settings(t.view(), &lc, run_keys_here, false); // (the same landscape: only the substrates changed)
    Run b;
    CHECK(make_run(b, t.c_str(), 1));
    const u32 want2 = world_substrates(b.world());
    CHECK(lc.land.substrates_distinct == want2 && want2 != want);
    // Every setting the merged builds added is read before the check stops
    // (World::init's check-only return), so the page shows its value and
    // checks it: their ranges and cross-checks come back by setting.
    t.clear();
    t.append(land);
    t.append("disaster.enabled = true\nworld.disk = 512\ndetect.shift = 65\ndisaster.reboot_min = 200\n");
    SettingsCheck nk;
    check_settings(t.view(), &nk, run_keys_here, false);
    const char* const added[] = {"substrate.smooth", "substrate.scale",      "substrate.share_two", "spark.activity",
                                 "world.archive",    "archive.slow",         "archive.rot",         "res.archive.spread",
                                 "detect.shift",     "disaster.surge_every", "disaster.grace",      "disaster.sounds"};
    bool all_read = true;
    for (const char* k : added) {
        bool found = false;
        for (usize i = 0; i < key_count() && !found; ++i)
            if (strcmp(key_at(i).name, k) == 0) {
                found = true;
                all_read = all_read && nk.effective(i).n > 0;
                if (!nk.effective(i).n)
                    fprintf(stderr, "  the check didn't read %s\n", k);
            }
        all_read = all_read && found;
    }
    CHECK(all_read);
    bool shift = false, reboot = false;
    for (usize i = 0; i < nk.problems.size(); ++i) {
        shift = shift || (strcmp(nk.problems[i].key, "detect.shift") == 0 && strstr(nk.problems[i].what, "64"));
        reboot = reboot || (strcmp(nk.problems[i].key, "disaster.reboot_min") == 0 &&
                            strstr(nk.problems[i].what, "must not exceed disaster.reboot_max"));
    }
    CHECK(shift && reboot);
    // Off (the test runner's legacy defaults), the summary says so.
    SettingsCheck off;
    check_settings("seed = 3\nworld.model = soup\nworld.graph = landscape\nworld.nodes = 150\nland.scale = 5\n"
                   "land.fraction = 0.3\nland.resolution = 96\nworld.soup_size = 1024\n",
                   &off, run_keys_here, false);
    CHECK(off.land.built && !off.land.substrates && off.land.substrates_distinct == 0);
    printf("  setup check: archive counted (%.0f bytes of RAM for 6 nodes); landscape substrates %u, %u at smooth 0.3\n",
           dc.ram, want, want2);
}

// --- files ---------------------------------------------------------------------------------

void test_page_save_load() {
    PageRig* a = new PageRig;
    CHECK(a->start(kWorld));
    CHECK(a->set("world.graph", "procedural") && a->set("world.nodes", "12") && a->set("heat.enabled", "false"));
    CHECK(a->set("seed.genome", "SENSE ALLOC") && a->set("hazard.copy", "0.0005")); // the default, as typed
    CHECK(a->set("spark.rate", "0.4"));
    TextBuf saved;
    a->page.save_text(saved);
    // Grouped and commented; what equals its default isn't there.
    CHECK(strstr(saved.c_str(), "# --- World and land ---") && strstr(saved.c_str(), "world.graph = procedural"));
    CHECK(strstr(saved.c_str(), "hazard.copy") == nullptr && strstr(saved.c_str(), "# Graph (default ring)"));
    PageRig* b = new PageRig;
    CHECK(b->start(""));
    TextBuf err;
    CHECK(b->page.load_text(saved.view(), "saved.cfg", &err));
    b->draw();
    bool same_all = true;
    for (usize k = 0; k < key_count(); ++k) {
        if (!same(key_at(k), a->page.shown(k), b->page.shown(k))) {
            fprintf(stderr, "  %s: '%.*s' saved, '%.*s' loaded\n", key_at(k).name, static_cast<int>(a->page.shown(k).n),
                    a->page.shown(k).p, static_cast<int>(b->page.shown(k).n), b->page.shown(k).p);
            same_all = false;
        }
    }
    CHECK(same_all && b->page.changed_count() == 0 && b->page.problem_count() == 0);
    // Through a file, as Save as and Load do it.
    const char* path = "nova_test_setup_saved.cfg";
    CHECK(a->page.save_file(path, &err));
    PageRig* c = new PageRig;
    CHECK(c->start(""));
    CHECK(c->page.load_file(path, &err));
    bool same_file = true;
    for (usize k = 0; k < key_count(); ++k)
        same_file = same_file && same(key_at(k), a->page.shown(k), c->page.shown(k));
    CHECK(same_file && strcmp(c->page.source(), path) == 0);
    remove(path);
    delete a;
    delete b;
    delete c;
}

// The world hash a nova process printed ("... hash 0123456789abcdef ...").
bool printed_hash(const char* out_path, u64* hash) {
    Array<u8> bytes;
    if (!read_file(out_path, &bytes))
        return false;
    bytes.push(0);
    const char* s = reinterpret_cast<const char*>(bytes.data());
    const char* last = nullptr;
    for (const char* p = strstr(s, "hash "); p; p = strstr(p + 1, "hash "))
        last = p;
    if (!last || strlen(last) < 21)
        return false;
    char hex[24] = "0x"; // parse_u64 reads 0x-hex
    memcpy(hex + 2, last + 5, 16);
    hex[18] = '\0';
    return parse_u64(Str(hex), hash);
}

void test_page_start_hash() {
    PageRig* r = new PageRig;
    TextBuf cfg;
    cfg.append(kSoupWorldProc);
    cfg.append("run.ticks = 40\nreplay.checkpoint_every = 10\n");
    CHECK(r->start(cfg.c_str()));
    CHECK(r->set("spark.rate", "0.4") && r->set("cpu.wear", "0.003"));
    CHECK(r->page.problem_count() == 0);
    const char* folder = "nova_test_setup_run";
    CreateDirectoryA(folder, nullptr);
    char cfg_path[200], out_path[200], png_path[200];
    snprintf(cfg_path, sizeof cfg_path, "%s\\run.cfg", folder);
    snprintf(out_path, sizeof out_path, "%s\\out.txt", folder);
    snprintf(png_path, sizeof png_path, "%s\\view.png", folder);
    remove(cfg_path);
    TextBuf text;
    r->page.run_cfg_text(folder, text);
    CHECK(write_file(cfg_path, text.view().p, text.size()));

    // Every effective setting is in it, as the run will read it.
    Config rc;
    Config::ParseError pe;
    CHECK(rc.parse(text.view(), &pe));
    const SettingsCheck& ch = r->page.check();
    u32 missing = 0, wrong = 0, written = 0;
    for (usize k = 0; k < key_count(); ++k) {
        const Key& key = key_at(k);
        const bool set = r->page.is_set(k), read = ch.read[k] != 0;
        if ((key.flags & kKeyHidden) && !set)
            continue;
        if ((key.flags & kKeyComputed) && !set && !read) {
            char line[160];
            snprintf(line, sizeof line, "# %s = ", key.name);
            missing += strstr(text.c_str(), line) == nullptr; // left for the run, as a comment
            continue;
        }
        ++written;
        bool found = false;
        for (usize i = 0; i < rc.count() && !found; ++i) {
            if (!rc.key(i).eq(key.name))
                continue;
            found = true;
            const Str want = set ? r->page.value(k) : (read ? ch.effective(k) : Str(key.def_text));
            if (!same(key, rc.value(i), want)) {
                fprintf(stderr, "  run.cfg: %s = %.*s, the run reads %.*s\n", key.name, static_cast<int>(rc.value(i).n),
                        rc.value(i).p, static_cast<int>(want.n), want.p);
                ++wrong;
            }
        }
        if (!found) {
            fprintf(stderr, "  run.cfg lacks %s\n", key.name);
            ++missing;
        }
    }
    CHECK(missing == 0 && wrong == 0 && rc.count() == written);
    CHECK(ch.read[r->key("land.aspect")] == 0); // a procedural world reads no landscape settings ...
    CHECK(rc.has("land.aspect"));               // ... and run.cfg still names them
    // A computed default nothing read is left for the run to work out, as a comment.
    CHECK(ch.read[r->key("world.chords")] == 0 && !rc.has("world.chords") &&
          strstr(text.c_str(), "# world.chords = ") != nullptr);

    // A run from run.cfg hashes as the settings it came from, at any thread count.
    Run* a = new Run;
    CHECK(a->config().load_file(cfg_path, &pe));
    TextBuf err;
    CHECK(a->init(4, &err));
    a->run_until(40);
    const u64 want = a->world().hash();
    delete a;
    TextBuf set_text;
    r->page.config_text(set_text);
    Run* b = new Run;
    CHECK(make_run(*b, set_text.c_str(), 2));
    b->run_until(40);
    CHECK_EQ_U64(b->world().hash(), want);
    delete b;

    // nova run on the file, and nova view on it (the window's own start), print the same hash.
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "\"\"%s\" run %s --no-esc --threads 3 > %s 2>&1\"", NOVA_EXE, cfg_path, out_path);
    CHECK(system(cmd) == 0);
    u64 run_hash = 0;
    CHECK(printed_hash(out_path, &run_hash));
    CHECK_EQ_U64(run_hash, want);
    snprintf(cmd, sizeof cmd, "\"\"%s\" view %s --no-output --ticks 40 --size 640x400 --render %s > %s 2>&1\"",
             NOVA_EXE, cfg_path, png_path, out_path);
    CHECK(system(cmd) == 0);
    u64 view_hash = 0;
    CHECK(printed_hash(out_path, &view_hash));
    CHECK_EQ_U64(view_hash, want);
    printf("  run.cfg: %u settings; a run from it, nova run and nova view all hash %016llx\n", written,
           static_cast<unsigned long long>(want));
    remove(out_path);
    remove(png_path);
    remove(cfg_path);
    RemoveDirectoryA(folder);
    delete r;
}

// --- drawing ----------------------------------------------------------------------------------

u32 colours(const Canvas& c) {
    u32 seen[64] = {};
    u32 kinds = 0;
    const u32 n = static_cast<u32>(c.width()) * static_cast<u32>(c.height());
    for (u32 i = 0; i < n; i += 31) {
        const u32 v = c.pixels()[i] & 0xFFFFFF;
        bool found = false;
        for (u32 k = 0; k < kinds && !found; ++k)
            found = seen[k] == v;
        if (!found && kinds < 64)
            seen[kinds++] = v;
    }
    return kinds;
}

void test_page_render() {
    PageRig* r = new PageRig;
    CHECK(r->start(kWorld));
    CHECK(r->set("spark.min_len", "100") && r->set("spark.max_len", "50"));
    r->show("Sparks");
    r->canvas.flush();
    CHECK(colours(r->canvas) >= 16);
    Rect row;
    CHECK(r->page.row_rect(r->key("spark.rate"), &row) && row.w > 400);
    // Every group draws.
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        r->page.select_group(g);
        r->draw();
    }
    // Read-only (a finished run): its controls take nothing; Copy makes it a new run's.
    r->page.set_mode(kSetupArchive, "runs\\finished");
    r->show("Heat");
    CHECK(r->page.read_only() && !r->page.can_start());
    const Rect c = r->ctl("heat.enabled");
    r->click(c.x + 8, c.y + c.h / 2);
    CHECK(r->is("heat.enabled", "true"));
    r->canvas.flush();
    CHECK(colours(r->canvas) >= 12);
    const i32 copy_x = 1600 - 8 - 84 - 6 - 75;
    r->click(copy_x, 6 + 13);
    CHECK(!r->page.read_only() && r->page.changed_count() == 0);
    r->click(c.x + 8, c.y + c.h / 2);
    CHECK(r->is("heat.enabled", "false"));
    // The live run's page: Back asks for the panels.
    r->page.set_mode(kSetupLive, "runs\\live");
    r->draw();
    r->page.requests().clear();
    r->click(1600 - 8 - 84 - 6 - 65, 6 + 13);
    CHECK(r->page.requests().size() == 1 && r->page.requests()[0].kind == kReqBack);
    delete r;
}

} // namespace

void setup_page_tests() {
    test_page_widgets();
    test_page_check();
    test_page_check_merged();
    test_page_save_load();
    test_page_start_hash();
    test_page_render();
}
