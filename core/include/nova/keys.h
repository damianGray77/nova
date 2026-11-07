#pragma once

// The settings registry (docs/config-plan.md C1): one entry for every setting
// the engine reads -- its name, kind, default, range, group, label and one
// line of help. The engine reads every setting through its entry, so each
// default and range lives in exactly one place, core/src/keys.inc, and the
// setup page and `nova keys` (docs/keys.md) can never fall behind the engine.
//
// ---------------------------------------------------------------------------
// ADDING A SETTING
// ---------------------------------------------------------------------------
// 1. Add one entry to core/src/keys.inc, in its group's section. One macro per
//    kind (help last, one plain sentence or two, drawn from docs/design.md):
//
//      NOVA_FLAG  (name, default,                 flags, group, label, needs, help)
//      NOVA_WHOLE (name, default, min, max,       flags, group, label, needs, help)
//      NOVA_NUMBER(name, default, min, max,       flags, group, label, needs, help)
//      NOVA_CHOICE(name, default, "opt1 opt2 ..", flags, group, label, needs, help)
//      NOVA_TEXT  (name, default,                 flags, group, label, needs, help)
//
//    For example:
//
//      NOVA_WHOLE("disaster.every", 0, 0, 1000000000, 0, "Disasters", "Disaster interval",
//                 "disaster.enabled", "Ticks between disasters on average (0 = never).")
//
//    - name:    the config key, [a-z0-9_.] dotted ("group.leaf").
//    - default, min, max: numbers (whole numbers are exact up to 2^53; use
//               kKeyNoMax for "no upper limit"). A flag's default is true or
//               false; a choice's and a text's default is a string.
//    - flags:   0, or any of these or'd together:
//                 kKeyLog          a rate spanning decades: the page's knob is
//                                  logarithmic (disk.rot, hazard.*, ...);
//                 kKeyHidden       test or demo only (demo.*): left off the page;
//                 kKeyComputed     the default depends on other settings: the
//                                  call site supplies it (key_default below);
//                                  the table's default is what it is with
//                                  every other setting at its own default, and
//                                  the help says how it is worked out;
//                 kKeyMaxComputed  the maximum depends on other settings (a
//                                  node's size, say): the call site supplies it
//                                  (key_max); the table's max is the largest it
//                                  can ever be;
//                 kKeyOpenMin      the value must be above min, not equal to it.
//    - group:   one of the groups in kKeyGroups (keys.cpp; docs/config-plan.md C2).
//    - label:   a few words for the page ("Disaster interval").
//    - needs:   the switch it depends on: "" (none), "key" (that switch on, or
//               that number or text non-zero / non-empty), or "key=a|b" (that
//               choice is a or b). The page greys the setting out otherwise.
//    - help:    one line for the page's tooltip and docs/keys.md.
//
// 2. Read it where the engine needs it, by name, with the reader for its kind:
//
//      const u32 every = key_u32(cfg, "disaster.every");
//
//    The reader takes the default and range from the entry, marks the key used
//    (typo safety: Config::first_unused), and reports a bad value through
//    cfg.fail, so the run refuses to start: out of range ("disaster.every:
//    must be between 0 and 1000000000") gives back the minimum, not one of a
//    choice's options ("must be a, b or c") its default, malformed text the
//    default. Cross-checks between settings stay at the call site
//    (cfg.fail("spark.min_len", "must not exceed spark.max_len")). Reading a
//    key with no entry, or with the wrong reader for its kind, is a bug: it
//    stops the process with "nova: fatal: config key '...' ..." naming the key
//    and the fix (tests stop there too).
//
// 3. Run `nova keys --md docs/keys.md` to refresh the reference (the keys
//    suite fails while docs/keys.md is stale), and the keys suite. It checks
//    every entry (a unique valid name, a known group, a label and help, the
//    default inside its range, needs naming a real switch), the families, and
//    that loading the presets (configs/*.cfg) reads every entry. If yours
//    is read only behind a switch the presets leave off, add a world that
//    reaches it to test_presets in tests/test_keys.cpp, as the custom-table
//    world there does. (tests/keys_seed.txt is the seeding pass's record of
//    the settings that existed before the registry: new settings don't go in
//    it.) A new group goes in kKeyGroups (keys.cpp), in page order.
//
// Families: a name with a <set> placeholder is a template, expanded once for
// every member of that set (NOVA_MEMBER(set, member, label, group) lines in
// keys.inc). "<set>" in the label and help becomes the member's label; an
// empty group takes the member's group. One member's own default is
// NOVA_DEFAULT(name, default). Read a member by its full name:
//
//      key_f64(cfg, "res.memory.spread")
//
// Computed parts, supplied by the call site (and only for entries so marked):
//
//      key_flag(cfg, "world.dispersal", key_default(spatial));
//      key_u32(cfg, "isa.search_limit", key_max(soup_size));
//      key_u32(cfg, "spark.max_len", key_default_max(min_of(128u, size), size));
//
// The readers, by kind:
//      flag    key_flag   -> bool
//      whole   key_u32 (max must fit 32 bits), key_u64, key_i64 (min below 0)
//      number  key_f64
//      choice  key_choice -> Str, always one of the options
//      text    key_str    -> Str
// and for any kind, key_text: the value as written, or the default's text
// (for a value parsed elsewhere, like the demo world's live knobs).
//
// Peeks, for estimates made before the world reads its settings (the
// budgets): key_peek_*, the value or the default, with no range check (the
// real read reports it).
//
// Fallback defaults (Config::set_fallback, the test runner's legacy defaults)
// still win over the registry's: a reader passes the registry's default to
// Config, which takes the fallback text first when the key is absent.
#include "nova/config.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

enum class KeyKind : u8 { flag, whole, number, choice, text };

inline constexpr u32 kKeyLog = 1u << 0;
inline constexpr u32 kKeyHidden = 1u << 1;
inline constexpr u32 kKeyComputed = 1u << 2;
inline constexpr u32 kKeyMaxComputed = 1u << 3;
inline constexpr u32 kKeyOpenMin = 1u << 4;

// "No upper limit" for a whole number or a number (2^64 as a double).
inline constexpr double kKeyNoMax = 18446744073709551616.0;
// KeyAt's "not supplied".
inline constexpr double kKeyUnset = -1.7976931348623157e308;

// One setting (a family member, expanded).
struct Key {
    const char* name;
    KeyKind kind;
    u32 flags;
    double def, lo, hi;   // flag: def 0 or 1; whole and number
    const char* def_text; // the default as a config value ("true", "4096", "1e-06", "ring", "")
    const char* options;  // choice: the options, space separated; else ""
    const char* group;
    const char* label;
    const char* needs;
    const char* help;
    const char* family;   // the template it was expanded from ("res.<res>.spread"), or nullptr
    const char* family_label; // ... and the template's label and help, placeholder and all
    const char* family_help;
};

// The groups, in page order (docs/config-plan.md C2).
extern const char* const kKeyGroups[];
extern const u32 kKeyGroupCount;

usize key_count();
const Key& key_at(usize i);
const Key* key_find(Str name); // nullptr if it has no entry
const char* key_kind_name(KeyKind k); // "switch", "whole", "number", "choice", "text"

// What the call site supplies for an entry marked kKeyComputed or
// kKeyMaxComputed (see the top of this file).
struct KeyAt {
    double def = kKeyUnset;
    double hi = kKeyUnset;
};
inline KeyAt key_default(double def) {
    KeyAt a;
    a.def = def;
    return a;
}
inline KeyAt key_max(double hi) {
    KeyAt a;
    a.hi = hi;
    return a;
}
inline KeyAt key_default_max(double def, double hi) {
    KeyAt a;
    a.def = def;
    a.hi = hi;
    return a;
}

// The readers (see the top of this file).
bool key_flag(Config& cfg, Str name, KeyAt at = {});
u32 key_u32(Config& cfg, Str name, KeyAt at = {});
u64 key_u64(Config& cfg, Str name, KeyAt at = {});
i64 key_i64(Config& cfg, Str name, KeyAt at = {});
double key_f64(Config& cfg, Str name, KeyAt at = {});
Str key_choice(Config& cfg, Str name);
Str key_str(Config& cfg, Str name);
Str key_text(Config& cfg, Str name);

// Peeks: the value as written (or the fallback's), else the entry's default;
// no range check. Marks the key used.
bool key_peek_flag(Config& cfg, Str name);
u64 key_peek_u64(Config& cfg, Str name);
double key_peek_f64(Config& cfg, Str name);
Str key_peek_str(Config& cfg, Str name);
// The same with the estimate's own assumption for a setting not given, in
// place of the entry's default. Only the budgets use it, for two settings
// whose estimates predate the registry (docs/results/registry.md); don't add
// more.
u64 key_peek_u64_assuming(Config& cfg, Str name, u64 assume);
double key_peek_f64_assuming(Config& cfg, Str name, double assume);

// Checks a value's text against the entry, as a reader would (the live
// knobs): false if malformed or out of range. A flag gives 0 or 1.
bool key_check(Str name, Str text, KeyAt at, double* out);

// A read that breaks the rules (no entry, the wrong reader, a computed part
// missing or not allowed) calls this hook with the key and the problem; the
// default stops the process (NOVA_FATAL). Tests swap it to observe. Returns
// the previous hook.
using KeyMisuseHook = void (*)(Str name, const char* what);
KeyMisuseHook key_set_misuse_hook(KeyMisuseHook hook);

// Every read through the registry, for the keys test: when set, each reader
// calls it with the entry it read. nullptr turns it off.
using KeyTraceHook = void (*)(const Key& key);
void key_set_trace_hook(KeyTraceHook hook);

// The setup page's check (docs/config-plan.md C4, nova/check.h): when set,
// each reader (not the peeks) also reports the value it gives back, as config
// text ("true", "4096", "1e-06", "ring"), so the page can show the value a run
// will actually use, computed defaults worked out. One process-wide hook, set
// only while the check runs; nullptr turns it off.
using KeyValueHook = void (*)(const Key& key, Str value);
void key_set_value_hook(KeyValueHook hook);

// Marks every key in cfg that has a registry entry as used, so that
// Config::first_unused afterwards reports only keys with no entry (typos). A
// key the registry knows that nothing read (its feature off) is not a typo:
// the setup page's run.cfg names every setting (docs/config-plan.md C3), and
// nova run and nova view accept it.
void key_accept_known(Config& cfg);

// `nova keys`: every setting as a plain-text table, by group.
void keys_table(TextBuf& out);
// `nova keys --md`: the Markdown reference (docs/keys.md).
void keys_markdown(TextBuf& out);

} // namespace nova
