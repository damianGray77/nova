// The settings registry (docs/config-plan.md C1, nova/keys.h): the table's
// entries and families; the seeding pass (every key the engine read before
// the registry has an entry with the same kind, default and range); every
// preset loading through it; fallback defaults still winning; unknown keys
// still failing; and `nova keys --md` (docs/keys.md) well formed and current.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // FindFirstFileA (the raw-read guard)

#include <cstdio>
#include <cstring>

#include "check.h"
#include "nova/budget.h"
#include "nova/config.h"
#include "nova/io.h"
#include "nova/keys.h"
#include "nova/soup.h"
#include "nova/world.h"

using namespace nova;

extern const char* const kLegacyDefaults; // test_main.cpp

namespace {

const char* const kPresets[] = {"abio", "archipelago", "demo", "gpu_world", "landscape", "maze", "soup", "world"};

bool is_group(const char* g) {
    for (u32 i = 0; i < kKeyGroupCount; ++i)
        if (strcmp(kKeyGroups[i], g) == 0)
            return true;
    return false;
}

// The words of a space-separated list, one at a time.
bool next_word(Str s, usize* at, Str* word) {
    while (*at < s.n && s.p[*at] == ' ')
        ++*at;
    if (*at >= s.n)
        return false;
    const usize b = *at;
    while (*at < s.n && s.p[*at] != ' ')
        ++*at;
    *word = Str(s.p + b, *at - b);
    return true;
}

bool has_option(const Key& k, Str v) {
    usize at = 0;
    Str w;
    while (next_word(Str(k.options), &at, &w))
        if (w.eq(v))
            return true;
    return false;
}

// "key", or "key=a|b": an existing switch, number or text, or a choice and
// some of its options.
bool needs_ok(const Key& k) {
    const Str nd(k.needs);
    if (nd.n == 0)
        return true;
    const usize eq = nd.find('=');
    const Key* on = key_find(nd.sub(0, eq));
    if (!on || on == &k)
        return false;
    if (eq == nd.n)
        return on->kind != KeyKind::choice;
    if (on->kind != KeyKind::choice)
        return false;
    Str rest = nd.sub(eq + 1, nd.n - eq - 1);
    while (rest.n) {
        const usize bar = rest.find('|');
        if (!has_option(*on, rest.sub(0, bar)))
            return false;
        rest = bar < rest.n ? rest.sub(bar + 1, rest.n - bar - 1) : Str();
    }
    return true;
}

// Every entry: a valid, unique name; a known group; a label and one line of
// help; a default inside its range; a choice's default among its options; a
// needs naming a real switch.
void test_entries() {
    const usize n = key_count();
    u32 bad = 0, hidden = 0, computed = 0, logs = 0, families = 0;
    for (usize i = 0; i < n; ++i) {
        const Key& k = key_at(i);
        bool ok = Config::valid_key(k.name) && strchr(k.name, '<') == nullptr && is_group(k.group) &&
                  k.label[0] && k.help[0] && !strchr(k.help, '\n') && strchr(k.label, '<') == nullptr &&
                  needs_ok(k);
        if (k.family) { // expanded: no placeholder left in its help
            const char* open = strchr(k.family, '<');
            const char* close = open ? strchr(open, '>') : nullptr;
            char ph[32] = {};
            if (open && close && close - open < 31)
                memcpy(ph, open, static_cast<usize>(close - open) + 1);
            ok = ok && ph[0] && !strstr(k.help, ph) && !strstr(k.needs, ph);
        }
        for (usize j = 0; j < i && ok; ++j)
            ok = strcmp(key_at(j).name, k.name) != 0; // no duplicates
        switch (k.kind) {
        case KeyKind::flag:
            ok = ok && (k.def == 0 || k.def == 1) && strcmp(k.def_text, k.def ? "true" : "false") == 0;
            break;
        case KeyKind::whole:
            ok = ok && k.lo <= k.def && k.def <= k.hi && static_cast<double>(static_cast<i64>(k.def)) == k.def &&
                 static_cast<double>(static_cast<i64>(k.lo)) == k.lo;
            break;
        case KeyKind::number:
            ok = ok && ((k.flags & kKeyOpenMin) ? k.lo < k.def : k.lo <= k.def) && k.def <= k.hi;
            break;
        case KeyKind::choice:
            ok = ok && k.options[0] && has_option(k, k.def_text) && !strstr(k.options, "  ");
            break;
        case KeyKind::text:
            ok = ok && !k.options[0];
            break;
        }
        if (k.kind != KeyKind::number)
            ok = ok && !(k.flags & (kKeyLog | kKeyOpenMin));
        if (k.kind != KeyKind::whole && k.kind != KeyKind::number)
            ok = ok && !(k.flags & kKeyMaxComputed);
        if (k.kind == KeyKind::choice || k.kind == KeyKind::text)
            ok = ok && !(k.flags & kKeyComputed);
        if (!ok) {
            fprintf(stderr, "  bad registry entry: %s\n", k.name);
            ++bad;
        }
        hidden += (k.flags & kKeyHidden) != 0;
        computed += (k.flags & (kKeyComputed | kKeyMaxComputed)) != 0;
        logs += (k.flags & kKeyLog) != 0;
        families += k.family && (i == 0 || key_at(i - 1).family != k.family);
    }
    CHECK(bad == 0);
    CHECK(n > 250);
    printf("  registry: %llu settings, %u families, %u hidden, %u computed, %u logarithmic\n",
           static_cast<unsigned long long>(n), families, hidden, computed, logs);
}

// Families are written once and expand for every member.
void test_families() {
    const char* const spreads[] = {"spark",   "res.memory", "res.cpu",  "res.gpu",
                                   "res.npu", "res.disk",   "res.ecpu", "res.archive"}; // (archive: W4)
    const char* const leaves[] = {"spread", "smooth", "barren", "hotspots", "hotspot_boost", "against"};
    u32 found = 0;
    for (const char* s : spreads) {
        for (const char* l : leaves) {
            char name[64];
            snprintf(name, sizeof name, "%s.%s", s, l);
            const Key* k = key_find(name);
            found += k && k->family && strncmp(k->family, "<spread>.", 9) == 0;
        }
    }
    CHECK(found == 48);
    CHECK(strcmp(key_find("spark.spread")->group, "Sparks") == 0);
    CHECK(strcmp(key_find("res.cpu.spread")->group, "Resources") == 0);
    CHECK(strcmp(key_find("res.gpu.against")->def_text, "none") == 0);
    CHECK(strcmp(key_find("res.memory.spread")->label, "Spread of memory") == 0);
    CHECK(strstr(key_find("res.memory.spread")->family_label, "<spread>") != nullptr);
    // Heat curves: one member's own default.
    CHECK(key_find("heat.cpu.hot")->def == 60 && key_find("heat.gpu.hot")->def == 60 &&
          key_find("heat.npu.hot")->def == 60);
    CHECK(key_find("heat.mem.hot")->def == 70 && strcmp(key_find("heat.mem.hot")->def_text, "70") == 0);
    CHECK(key_find("heat.mem.warm")->def == 40 && key_find("heat.npu.err_max")->def == 10);
    CHECK(strcmp(key_find("heat.gpu.speed_min")->family, "heat.<unit>.speed_min") == 0);
    // vary.*: the resource fields are procedural-only (a landscape has distributions).
    CHECK(strcmp(key_find("vary.soup_size")->needs, "world.graph=procedural") == 0);
    CHECK(strcmp(key_find("vary.cool")->needs, "world.graph=procedural|landscape") == 0);
    u32 vary = 0, was = 0;
    for (usize i = 0; i < key_count(); ++i) {
        vary += Str(key_at(i).name).starts_with("vary.");
        was += Str(key_at(i).name).starts_with("budget.") && strstr(key_at(i).name, "_was") &&
               (key_at(i).flags & kKeyHidden);
    }
    CHECK(vary == 11 && was == 4);
    // Plain entries aren't family members.
    CHECK(key_find("world.nodes")->family == nullptr && key_find("world.nodes")->family_label == nullptr);
}

// The seeding pass (tests/keys_seed.txt): every key the engine read before
// the registry, with the reader, default and range check its call site had.
// The registry must agree with every line: the same kind, the same default
// (unless it is computed, or a budget's own assumption) and the same range
// (unless the maximum is computed).
bool parse_num(Str s, double* out) {
    if (s.n && s.p[0] == '-') {
        i64 v;
        if (parse_i64(s, &v)) {
            *out = static_cast<double>(v);
            return true;
        }
    }
    return parse_f64(s, out);
}

void test_seed() {
    char path[600];
    snprintf(path, sizeof path, "%s/tests/keys_seed.txt", NOVA_SOURCE_DIR);
    Array<u8> bytes;
    CHECK(read_file(path, &bytes));
    const Str text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    u32 lines = 0, bad = 0;
    usize at = 0;
    while (at < text.n) {
        usize end = at;
        while (end < text.n && text.p[end] != '\n')
            ++end;
        Str line = text.sub(at, end - at);
        at = end + 1;
        if (line.n && line.p[line.n - 1] == '\r')
            line = line.sub(0, line.n - 1);
        if (line.n == 0 || line.p[0] == '#')
            continue;
        Str f[6];
        u32 nf = 0;
        usize b = 0;
        for (usize i = 0; i <= line.n && nf < 6; ++i) {
            if (i == line.n || line.p[i] == '\t') {
                f[nf++] = line.sub(b, i - b);
                b = i + 1;
            }
        }
        ++lines;
        const Key* k = nf >= 5 ? key_find(f[0]) : nullptr;
        bool ok = k != nullptr;
        if (ok) {
            const Str reader = f[1], def = f[2], lo = f[3], hi = f[4], tag = nf > 5 ? f[5] : Str();
            const bool any_text = k->kind == KeyKind::choice || k->kind == KeyKind::text;
            // The kind.
            if (reader.eq("u64"))
                ok = k->kind == KeyKind::whole && k->lo >= 0;
            else if (reader.eq("i64"))
                ok = k->kind == KeyKind::whole;
            else if (reader.eq("f64"))
                ok = k->kind == KeyKind::number;
            else if (reader.eq("bool"))
                ok = k->kind == KeyKind::flag;
            else if (reader.eq("str"))
                ok = true; // any kind can be read as text (the demo world's live knobs)
            else
                ok = false;
            // The default.
            const bool check_def = !(k->flags & kKeyComputed) && !tag.eq("assumed");
            if (ok && check_def) {
                if (reader.eq("str") || any_text) {
                    ok = def.eq(k->def_text);
                } else if (reader.eq("bool")) {
                    ok = def.eq(k->def_text);
                } else {
                    double d = 0;
                    ok = parse_num(def, &d) && d == k->def;
                }
            }
            // The range.
            double v = 0;
            if (ok && !lo.eq("-"))
                ok = parse_num(lo, &v) && v == k->lo;
            if (ok && !hi.eq("-") && !(k->flags & kKeyMaxComputed))
                ok = parse_num(hi, &v) && v == k->hi;
            if (ok && hi.eq("-") && !lo.eq("-")) // no maximum: none in the registry either
                ok = k->hi >= kKeyNoMax;
            if (ok)
                ok = tag.eq("openmin") == ((k->flags & kKeyOpenMin) != 0);
        }
        if (!ok) {
            fprintf(stderr, "  seed line disagrees with the registry: %.*s\n", static_cast<int>(line.n), line.p);
            ++bad;
        }
    }
    CHECK(lines > 350);
    CHECK(bad == 0);
    printf("  seeding pass: %u recorded reads, every one matches its entry\n", lines);
}

// The keys every read went through, while the hook is on.
const Key* g_read[1024];
u32 g_reads = 0;

void note_read(const Key& k) {
    for (u32 i = 0; i < g_reads; ++i)
        if (g_read[i] == &k)
            return;
    if (g_reads < 1024)
        g_read[g_reads++] = &k;
}

// What `nova run` reads before the world (app/main.cpp read_run_keys), so the
// typo check below sees the same config a run does.
void read_run_keys_here(Config& cfg) {
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
    const char* const outputs[] = {"csv",      "nodes",  "signals",  "replay",  "snapshot",
                                   "genebank", "lineages", "result", "profiles", "arrivals"}; // (A3: the last two)
    for (const char* o : outputs) {
        char name[32];
        snprintf(name, sizeof name, "output.%s", o);
        (void)key_str(cfg, name);
    }
}

// Every preset loads through the registry as a run would (budgets, then the
// world, with real defaults: no test fallback), every key in it has an entry,
// and nothing in it goes unread.
bool load_preset(const char* name) {
    char path[600];
    snprintf(path, sizeof path, "%s/configs/%s.cfg", NOVA_SOURCE_DIR, name);
    Config cfg;
    Config::ParseError pe;
    if (!cfg.load_file(path, &pe)) {
        fprintf(stderr, "  %s: %s\n", path, pe.msg);
        return false;
    }
    bool ok = true;
    for (usize i = 0; i < cfg.count(); ++i) {
        if (!key_find(cfg.key(i))) {
            fprintf(stderr, "  %s: '%.*s' has no registry entry\n", name, static_cast<int>(cfg.key(i).n), cfg.key(i).p);
            ok = false;
        }
    }
    read_run_keys_here(cfg);
    // Kept light: no real devices, at most 200 nodes (the same keys are read).
    if (key_peek_str(cfg, "world.model").eq("soup")) {
        cfg.set("accel.gpu", "cpu");
        cfg.set("accel.npu", "cpu");
        cfg.set("warp.device", "cpu");
    }
    if (key_peek_u64(cfg, "world.nodes") > 200)
        cfg.set("world.nodes", "200");
    TextBuf notes, err;
    if (!resolve_budgets(cfg, 1, &notes, &err)) {
        fprintf(stderr, "  %s: budgets: %s\n", name, err.c_str());
        return false;
    }
    World* w = new World;
    if (!w->init(cfg)) {
        fprintf(stderr, "  %s: %s\n", name, cfg.value_error());
        ok = false;
    }
    delete w;
    Str unused;
    if (cfg.first_unused(&unused)) {
        fprintf(stderr, "  %s: '%.*s' is not used by anything\n", name, static_cast<int>(unused.n), unused.p);
        ok = false;
    }
    return ok;
}

void test_presets() {
    CHECK(Config::set_fallback(nullptr, nullptr));
    g_reads = 0;
    key_set_trace_hook(note_read);
    for (const char* p : kPresets)
        CHECK(load_preset(p));
    const u32 by_presets = g_reads;
    // ... and the one setting no preset reaches: a custom codon table.
    CodonTable t;
    CHECK(builtin_table("nova_v1", &t));
    TextBuf text;
    text.append("world.model = soup\nworld.nodes = 1\nworld.soup_size = 1024\naccel.gpu = cpu\naccel.npu = cpu\n"
                "warp.device = cpu\nisa.table = custom\nisa.codons =");
    for (u32 c = 0; c < 64; ++c) {
        text.append_char(' ');
        text.append(op_name(t.op[c]));
    }
    text.append_char('\n');
    Config cfg;
    Config::ParseError pe;
    CHECK(cfg.parse(text.view(), &pe));
    World* w = new World;
    CHECK(w->init(cfg));
    delete w;
    key_set_trace_hook(nullptr);
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
    // No entry is unreachable: every one was read on the way.
    u32 unread = 0;
    for (usize i = 0; i < key_count(); ++i) {
        bool seen = false;
        for (u32 j = 0; j < g_reads && !seen; ++j)
            seen = g_read[j] == &key_at(i);
        if (!seen) {
            fprintf(stderr, "  no preset or custom-table world reads %s\n", key_at(i).name);
            ++unread;
        }
    }
    CHECK(unread == 0);
    printf("  presets: %u distinct settings read through the registry loading all %u (all %u with a custom "
           "table)\n",
           by_presets, static_cast<unsigned>(sizeof kPresets / sizeof kPresets[0]), g_reads);
}

// The test runner's fallback defaults (the legacy ones) still win over the
// registry's, computed defaults included; a config's own value wins over both.
void test_fallback() {
    Config c;
    Config::ParseError pe;
    CHECK(c.parse("", &pe));
    // Under the legacy fallback (set by test_main.cpp).
    CHECK(key_choice(c, "accel.mode").eq("instant"));                   // registry: async
    CHECK(key_u32(c, "world.gpus") == 0);                                // registry: 400
    CHECK(key_f64(c, "disk.rot", key_default(1e-8)) == 0.000001);        // computed, still the fallback's
    CHECK(!key_flag(c, "life.dormancy"));                                // registry: true
    CHECK(key_f64(c, "heat.cool") == 0.1);                               // not in the fallback: the registry's
    SoupParams* p = new SoupParams;
    CHECK(soup_params_from_config(c, 1024, p));
    CHECK(!p->accel_async && p->gpus == 0 && strcmp(p->table.name, "nova_v1") == 0);
    // Another fallback; the config's own value beats it.
    CHECK(Config::set_fallback("heat.cool = 0.5\nworld.nodes = 7\n", &pe));
    CHECK(key_f64(c, "heat.cool") == 0.5 && key_u32(c, "world.nodes") == 7);
    Config d;
    CHECK(d.parse("heat.cool = 0.2\n", &pe));
    CHECK(key_f64(d, "heat.cool") == 0.2);
    // No fallback: the registry's defaults.
    CHECK(Config::set_fallback(nullptr, nullptr));
    CHECK(key_choice(c, "accel.mode").eq("async") && key_u32(c, "world.gpus") == 400 && key_flag(c, "life.dormancy"));
    CHECK(soup_params_from_config(c, 1024, p));
    CHECK(p->accel_async && p->gpus == 400 && strcmp(p->table.name, "nova_v2") == 0 && p->dormancy);
    delete p;
    CHECK(Config::set_fallback(kLegacyDefaults, nullptr));
}

Str g_misused;
u32 g_misuses = 0;
char g_misused_buf[128];

void note_misuse(Str name, const char* what) {
    (void)what;
    const usize n = name.n < sizeof g_misused_buf ? name.n : sizeof g_misused_buf - 1;
    memcpy(g_misused_buf, name.p, n);
    g_misused = Str(g_misused_buf, n);
    ++g_misuses;
}

bool misused(const char* name, u32 before) { return g_misuses == before + 1 && g_misused.eq(name); }

// A key in a config that nothing reads still fails as unused (typo safety);
// a read of a key with no entry, with the wrong reader, or without its
// computed part is caught; bad values report through cfg.fail as before.
void test_unknown() {
    Config c;
    Config::ParseError pe;
    CHECK(c.parse("world.model = soup\nworld.nodes = 2\nworld.ndoes = 5\nworld.soup_size = 1024\n", &pe));
    World* w = new World;
    CHECK(w->init(c));
    delete w;
    Str k;
    CHECK(c.first_unused(&k) && k.eq("world.ndoes"));

    const KeyMisuseHook prev = key_set_misuse_hook(note_misuse);
    u32 n = g_misuses;
    CHECK(key_u32(c, "world.ndoes") == 0 && misused("world.ndoes", n)); // no entry
    n = g_misuses;
    (void)key_f64(c, "world.nodes"); // a whole number read as a number
    CHECK(misused("world.nodes", n));
    n = g_misuses;
    (void)key_u32(c, "world.chords"); // computed: the call site must give the default
    CHECK(misused("world.chords", n));
    n = g_misuses;
    (void)key_u32(c, "world.nodes", key_default(5)); // not computed
    CHECK(misused("world.nodes", n));
    n = g_misuses;
    (void)key_u32(c, "isa.search_limit"); // the maximum is computed
    CHECK(misused("isa.search_limit", n));
    n = g_misuses;
    (void)key_u32(c, "seed"); // over 32 bits
    CHECK(misused("seed", n));
    n = g_misuses;
    (void)key_u64(c, "run.after_replicator"); // can be negative
    CHECK(misused("run.after_replicator", n));
    n = g_misuses;
    double d = 0;
    CHECK(!key_check("spark.min_len", "8", KeyAt{}, &d) && misused("spark.min_len", n));
    n = g_misuses;
    (void)key_peek_u64(c, "world.chords"); // a computed default can't be peeked
    CHECK(misused("world.chords", n));
    key_set_misuse_hook(prev);
    CHECK(c.ok()); // none of that touched the config's error

    // Values out of range, or not among a choice's options: cfg.fail, the minimum (or the default) back.
    Config e;
    CHECK(e.parse("world.nodes = 0\n", &pe));
    CHECK(key_u32(e, "world.nodes") == 1 && strstr(e.value_error(), "world.nodes: must be between 1 and 1000000"));
    Config f;
    CHECK(f.parse("world.graph = torus\n", &pe));
    CHECK(key_choice(f, "world.graph").eq("ring") &&
          strstr(f.value_error(), "world.graph: must be ring, procedural or landscape"));
    Config g;
    CHECK(g.parse("detect.similarity = 0\n", &pe));
    (void)key_f64(g, "detect.similarity");
    CHECK(strstr(g.value_error(), "must be above 0 and at most 1") != nullptr);
    Config h;
    CHECK(h.parse("run.after_replicator = -2\nrun.seconds = -1\n", &pe));
    CHECK(key_i64(h, "run.after_replicator") == -1 && strstr(h.value_error(), "must be -1 or more"));
    Config i;
    CHECK(i.parse("run.seconds = -1\nisa.search_limit = 300\n", &pe));
    (void)key_f64(i, "run.seconds");
    CHECK(strstr(i.value_error(), "run.seconds: must be at least 0") != nullptr);
    Config j;
    CHECK(j.parse("isa.search_limit = 300\nseed = 18446744073709551615\n", &pe));
    CHECK(key_u32(j, "isa.search_limit", key_max(256)) == 1 &&
          strstr(j.value_error(), "must be between 1 and 256") != nullptr);
    CHECK(key_u64(j, "seed") == 18446744073709551615ull); // no upper limit
    // The live knobs' check, the same ranges.
    CHECK(key_check("spark.rate", "2", KeyAt{}, &d) && d == 2);
    CHECK(!key_check("spark.rate", "-1", KeyAt{}, &d) && !key_check("spark.rate", "x", KeyAt{}, &d));
    CHECK(key_check("spark.min_len", "100", key_max(100), &d) && !key_check("spark.min_len", "101", key_max(100), &d));
    CHECK(!key_check("spark.grant", "0", KeyAt{}, &d) && key_check("demo.send_rate", "1", KeyAt{}, &d));
}

// nova keys: the plain table names every setting; the Markdown reference is
// well formed (every table row has its header's columns, every group with
// visible settings has its section, every setting appears) and docs/keys.md
// is what it generates.
void test_markdown() {
    TextBuf t;
    keys_table(t);
    for (usize i = 0; i < key_count(); ++i)
        CHECK(strstr(t.c_str(), key_at(i).name) != nullptr);

    TextBuf md;
    keys_markdown(md);
    const char* s = md.c_str();
    CHECK(Str(s).starts_with("# Settings reference\n"));
    // Tables: every row has as many cells as its header; a separator follows each header.
    u32 rows = 0, bad = 0, cols = 0;
    bool expect_sep = false;
    const Str all = md.view();
    usize at = 0;
    while (at < all.n) {
        usize end = at;
        while (end < all.n && all.p[end] != '\n')
            ++end;
        const Str line = all.sub(at, end - at);
        at = end + 1;
        if (line.n == 0 || line.p[0] != '|') {
            cols = 0;
            if (expect_sep)
                ++bad;
            expect_sep = false;
            continue;
        }
        u32 bars = 0;
        for (usize i = 0; i < line.n; ++i)
            bars += line.p[i] == '|' && (i == 0 || line.p[i - 1] != '\\');
        if (line.p[line.n - 1] != '|')
            ++bad;
        if (cols == 0) { // a header
            cols = bars;
            expect_sep = true;
            continue;
        }
        if (expect_sep) {
            bad += !line.starts_with("|---|");
            expect_sep = false;
        }
        bad += bars != cols;
        ++rows;
    }
    CHECK(bad == 0);
    // Every group with visible settings has its section, and the empty ones none.
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        bool visible = false;
        for (usize i = 0; i < key_count(); ++i)
            visible = visible || (strcmp(key_at(i).group, kKeyGroups[g]) == 0 && !(key_at(i).flags & kKeyHidden));
        char heading[96];
        snprintf(heading, sizeof heading, "\n## %s\n", kKeyGroups[g]);
        CHECK((strstr(s, heading) != nullptr) == visible);
    }
    // Every setting, by its name or its family's template.
    u32 missing = 0;
    for (usize i = 0; i < key_count(); ++i) {
        const Key& k = key_at(i);
        char a[160], b[160];
        snprintf(a, sizeof a, "`%s`", k.name);
        snprintf(b, sizeof b, "`%s`", k.family ? k.family : k.name);
        missing += !strstr(s, a) && !strstr(s, b);
    }
    CHECK(missing == 0);
    CHECK(rows > 100);
    // docs/keys.md is current.
    char path[600];
    snprintf(path, sizeof path, "%s/docs/keys.md", NOVA_SOURCE_DIR);
    Array<u8> bytes;
    bool same = read_file(path, &bytes);
    if (same) {
        usize j = 0;
        for (usize i = 0; i < bytes.size() && same; ++i) {
            if (bytes[i] == '\r')
                continue;
            same = j < all.n && static_cast<char>(bytes[i]) == all.p[j++];
        }
        same = same && j == all.n;
    }
    if (!same)
        fprintf(stderr, "  docs/keys.md is stale: nova keys --md docs/keys.md\n");
    CHECK(same);
    printf("  nova keys --md: %u table rows, well formed; docs/keys.md current\n", rows);
}

// The engine reads through the registry only: outside keys.cpp (and Config
// itself), no source in core/ or app/ reads a setting with Config's raw
// getters, so no default or range can live anywhere but keys.inc.
void test_no_raw_reads() {
    const char* const dirs[] = {"core/src", "app"};
    const char* const raw[] = {"get_u64(\"", "get_i64(\"", "get_f64(\"", "get_bool(\"", "get_str(\"", "cfg.get_",
                               "config().get_"};
    u32 files = 0, bad = 0;
    for (const char* dir : dirs) {
        char pattern[600];
        snprintf(pattern, sizeof pattern, "%s/%s/*.cpp", NOVA_SOURCE_DIR, dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (strcmp(fd.cFileName, "keys.cpp") == 0 || strcmp(fd.cFileName, "config.cpp") == 0)
                continue;
            char path[800];
            snprintf(path, sizeof path, "%s/%s/%s", NOVA_SOURCE_DIR, dir, fd.cFileName);
            Array<u8> bytes;
            if (!read_file(path, &bytes))
                continue;
            bytes.push(0);
            ++files;
            const char* text = reinterpret_cast<const char*>(bytes.data());
            for (const char* r : raw) {
                if (strstr(text, r)) {
                    fprintf(stderr, "  %s/%s reads a setting without the registry (%s...)\n", dir, fd.cFileName, r);
                    ++bad;
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    CHECK(files > 20);
    CHECK(bad == 0);
}

} // namespace

void suite_keys() {
    test_entries();
    test_families();
    test_seed();
    test_presets();
    test_fallback();
    test_unknown();
    test_markdown();
    test_no_raw_reads();
}
