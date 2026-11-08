#include "nova/keys.h"

#include <cstdio>  // snprintf
#include <cstring> // strlen, memcpy, strcmp

namespace nova {

const char* const kKeyGroups[] = {"Run and output",
                                  "World and land",
                                  "Paths and bridges",
                                  "Resources",
                                  "Substrates",
                                  "Sparks",
                                  "CPU and cores",
                                  "Memory",
                                  "Disk",
                                  "GPU",
                                  "NPU",
                                  "Heat",
                                  "Hazards",
                                  "Costs",
                                  "Contact and eating",
                                  "Signals",
                                  "Life and death",
                                  "Dormancy",
                                  "Disasters",
                                  "Detection and lineages",
                                  "Budgets"};
const u32 kKeyGroupCount = sizeof kKeyGroups / sizeof kKeyGroups[0];

namespace {

// ---------------------------------------------------------------------------
// The table (keys.inc), as written: templates not yet expanded
// ---------------------------------------------------------------------------

struct Def {
    const char* name;
    KeyKind kind;
    double def, lo, hi;
    const char* text; // choice, text: the default
    const char* options;
    u32 flags;
    const char* group;
    const char* label;
    const char* needs;
    const char* help;
};

struct Member {
    const char* set;
    const char* key;
    const char* label;
    const char* group;
};

struct Override {
    const char* name;
    double def;
};

#define NOVA_FLAG(n, d, f, g, l, nd, h) {n, KeyKind::flag, (d) ? 1.0 : 0.0, 0.0, 1.0, "", "", f, g, l, nd, h},
#define NOVA_WHOLE(n, d, lo, hi, f, g, l, nd, h)                                                                     \
    {n, KeyKind::whole, static_cast<double>(d), static_cast<double>(lo), static_cast<double>(hi), "", "", f, g, l, nd, h},
#define NOVA_NUMBER(n, d, lo, hi, f, g, l, nd, h)                                                                    \
    {n, KeyKind::number, static_cast<double>(d), static_cast<double>(lo), static_cast<double>(hi), "", "", f, g, l, nd, h},
#define NOVA_CHOICE(n, d, o, f, g, l, nd, h) {n, KeyKind::choice, 0.0, 0.0, 0.0, d, o, f, g, l, nd, h},
#define NOVA_TEXT(n, d, f, g, l, nd, h) {n, KeyKind::text, 0.0, 0.0, 0.0, d, "", f, g, l, nd, h},
#define NOVA_MEMBER(s, m, l, g)
#define NOVA_DEFAULT(n, d)
const Def kDefs[] = {
#include "keys.inc"
};
#undef NOVA_FLAG
#undef NOVA_WHOLE
#undef NOVA_NUMBER
#undef NOVA_CHOICE
#undef NOVA_TEXT
#undef NOVA_MEMBER
#undef NOVA_DEFAULT

#define NOVA_FLAG(n, d, f, g, l, nd, h)
#define NOVA_WHOLE(n, d, lo, hi, f, g, l, nd, h)
#define NOVA_NUMBER(n, d, lo, hi, f, g, l, nd, h)
#define NOVA_CHOICE(n, d, o, f, g, l, nd, h)
#define NOVA_TEXT(n, d, f, g, l, nd, h)
#define NOVA_MEMBER(s, m, l, g) {s, m, l, g},
#define NOVA_DEFAULT(n, d)
const Member kMembers[] = {
#include "keys.inc"
};
#undef NOVA_MEMBER
#undef NOVA_DEFAULT

#define NOVA_MEMBER(s, m, l, g)
#define NOVA_DEFAULT(n, d) {n, static_cast<double>(d)},
const Override kOverrides[] = {
#include "keys.inc"
};
#undef NOVA_FLAG
#undef NOVA_WHOLE
#undef NOVA_NUMBER
#undef NOVA_CHOICE
#undef NOVA_TEXT
#undef NOVA_MEMBER
#undef NOVA_DEFAULT

// ---------------------------------------------------------------------------
// The registry: every entry, families expanded (built once, on first use)
// ---------------------------------------------------------------------------

u64 hash_name(Str s) {
    u64 h = 0xcbf29ce484222325ull; // FNV-1a
    for (usize i = 0; i < s.n; ++i) {
        h ^= static_cast<u8>(s.p[i]);
        h *= 0x100000001b3ull;
    }
    return h;
}

// A string that lives as long as the process (the registry is never freed).
const char* keep(Str s) {
    char* p = new char[s.n + 1];
    if (s.n)
        memcpy(p, s.p, s.n);
    p[s.n] = '\0';
    return p;
}

// s with every `from` replaced by `to`.
const char* replace(const char* s, Str from, Str to) {
    TextBuf t;
    const usize n = strlen(s);
    for (usize i = 0; i < n;) {
        if (from.n && i + from.n <= n && memcmp(s + i, from.p, from.n) == 0) {
            t.append(to);
            i += from.n;
        } else {
            t.append_char(s[i++]);
        }
    }
    return keep(t.view());
}

const char* default_text(KeyKind kind, double def, const char* text) {
    TextBuf t;
    switch (kind) {
    case KeyKind::flag:
        return def != 0 ? "true" : "false";
    case KeyKind::whole:
        if (def < 0)
            t.append_i64(static_cast<i64>(def));
        else
            t.append_u64(static_cast<u64>(def));
        return keep(t.view());
    case KeyKind::number:
        t.append_f64(def);
        return keep(t.view());
    case KeyKind::choice:
    case KeyKind::text:
        return text;
    }
    return "";
}

struct Registry {
    Array<Key> keys;
    Array<u64> hashes;

    Registry() {
        for (const Def& d : kDefs) {
            const char* open = strchr(d.name, '<');
            if (!open) {
                add(d, d.name, d.label, d.help, d.needs, d.group, nullptr);
                continue;
            }
            const char* close = strchr(open, '>');
            if (!close)
                NOVA_FATAL("keys.inc: a family name's <set> is not closed");
            const Str ph(open, static_cast<usize>(close - open) + 1); // "<set>"
            const Str set(open + 1, static_cast<usize>(close - open) - 1);
            u32 members = 0;
            for (const Member& m : kMembers) {
                if (!set.eq(m.set))
                    continue;
                ++members;
                add(d, replace(d.name, ph, m.key), replace(d.label, ph, m.label), replace(d.help, ph, m.label),
                    replace(d.needs, ph, m.key), d.group[0] ? d.group : m.group, &d);
            }
            if (!members)
                NOVA_FATAL("keys.inc: a family's set has no NOVA_MEMBER lines");
        }
        for (const Override& o : kOverrides) {
            Key* k = nullptr;
            for (usize i = 0; i < keys.size() && !k; ++i)
                if (strcmp(keys[i].name, o.name) == 0)
                    k = &keys[i];
            if (!k || !k->family)
                NOVA_FATAL("keys.inc: NOVA_DEFAULT names no family member");
            k->def = o.def;
            k->def_text = default_text(k->kind, o.def, "");
        }
    }

    void add(const Def& d, const char* name, const char* label, const char* help, const char* needs,
             const char* group, const Def* family) {
        Key k;
        k.name = name;
        k.kind = d.kind;
        k.flags = d.flags;
        k.def = d.def;
        k.lo = d.lo;
        k.hi = d.hi;
        k.def_text = default_text(d.kind, d.def, d.text);
        k.options = d.options;
        k.group = group;
        k.label = label;
        k.needs = needs;
        k.help = help;
        k.family = family ? family->name : nullptr;
        k.family_label = family ? family->label : nullptr;
        k.family_help = family ? family->help : nullptr;
        keys.push(k);
        hashes.push(hash_name(name));
    }
};

const Registry& reg() {
    static const Registry r; // built once (thread-safe static initialisation)
    return r;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

KeyMisuseHook g_misuse = nullptr;
KeyTraceHook g_trace = nullptr;
KeyValueHook g_value = nullptr; // the setup page's check (key_set_value_hook)

// The value a reader gives back, to the value hook (when set).
void report(const Key& k, Str v) {
    if (g_value)
        g_value(k, v);
}

void report_u(const Key& k, u64 v) {
    if (!g_value)
        return;
    TextBuf t;
    t.append_u64(v);
    g_value(k, t.view());
}

void report_i(const Key& k, i64 v) {
    if (!g_value)
        return;
    TextBuf t;
    t.append_i64(v);
    g_value(k, t.view());
}

void report_f(const Key& k, double v) {
    if (!g_value)
        return;
    TextBuf t;
    t.append_f64(v);
    g_value(k, t.view());
}

int show(Str s) { return s.n > 96 ? 96 : static_cast<int>(s.n); }

void misuse(Str name, const char* what) {
    if (g_misuse) {
        g_misuse(name, what);
        return;
    }
    char msg[320];
    snprintf(msg, sizeof msg, "config key '%.*s' %s", show(name), name.p, what);
    NOVA_FATAL(msg);
}

// The entry for a read, or nullptr (after reporting) if the read breaks the
// rules: no entry, the wrong kind, or a computed part missing or not allowed.
const Key* entry(Str name, KeyKind kind, KeyAt at) {
    const Key* k = key_find(name);
    if (!k) {
        misuse(name, "has no entry in the settings registry (add one to core/src/keys.inc)");
        return nullptr;
    }
    if (k->kind != kind) {
        char what[128];
        snprintf(what, sizeof what, "is a %s setting: read it with its own reader (nova/keys.h), not a %s one",
                 key_kind_name(k->kind), key_kind_name(kind));
        misuse(name, what);
        return nullptr;
    }
    const bool want_def = (k->flags & kKeyComputed) != 0, has_def = at.def != kKeyUnset;
    if (want_def != has_def) {
        misuse(name, want_def ? "has a computed default: the call site must supply it (key_default)"
                              : "is not marked kKeyComputed: its default comes from the registry");
        return nullptr;
    }
    const bool want_hi = (k->flags & kKeyMaxComputed) != 0, has_hi = at.hi != kKeyUnset;
    if (want_hi != has_hi) {
        misuse(name, want_hi ? "has a computed maximum: the call site must supply it (key_max)"
                             : "is not marked kKeyMaxComputed: its maximum comes from the registry");
        return nullptr;
    }
    if (g_trace)
        g_trace(*k);
    return k;
}

double def_of(const Key& k, KeyAt at) { return at.def != kKeyUnset ? at.def : k.def; }
double hi_of(const Key& k, KeyAt at) { return at.hi != kKeyUnset ? at.hi : k.hi; }

bool has_option(const char* options, Str v) {
    const usize n = strlen(options);
    usize i = 0;
    while (i < n) {
        usize j = i;
        while (j < n && options[j] != ' ')
            ++j;
        if (v.eq(Str(options + i, j - i)))
            return true;
        i = j + 1;
    }
    return false;
}

// "must be a, b or c", from a choice's options.
void options_message(const char* options, char* out, usize cap) {
    TextBuf t;
    t.append("must be ");
    const usize n = strlen(options);
    usize words = 0;
    for (usize i = 0; i < n; ++i)
        words += options[i] == ' ';
    ++words;
    usize w = 0, i = 0;
    while (i < n) {
        usize j = i;
        while (j < n && options[j] != ' ')
            ++j;
        if (w > 0)
            t.append(w + 1 == words ? " or " : ", ");
        t.append(Str(options + i, j - i));
        ++w;
        i = j + 1;
    }
    snprintf(out, cap, "%.*s", static_cast<int>(t.size()), t.view().p);
}

// The range check's message, as the getters before the registry wrote it.
void range_message_u(u64 lo, double hi, char* out, usize cap) {
    if (hi >= kKeyNoMax)
        snprintf(out, cap, "must be at least %llu", static_cast<unsigned long long>(lo));
    else
        snprintf(out, cap, "must be between %llu and %llu", static_cast<unsigned long long>(lo),
                 static_cast<unsigned long long>(hi));
}

void range_message_f(const Key& k, double hi, char* out, usize cap) {
    const bool open = (k.flags & kKeyOpenMin) != 0;
    if (hi >= kKeyNoMax)
        snprintf(out, cap, open ? "must be above %g" : "must be at least %g", k.lo);
    else
        snprintf(out, cap, open ? "must be above %g and at most %g" : "must be between %g and %g", k.lo, hi);
}

bool in_range_f(const Key& k, double v, double hi) {
    const bool above = (k.flags & kKeyOpenMin) ? v > k.lo : v >= k.lo;
    return above && v <= hi;
}

bool in_range_u(const Key& k, u64 v, double hi) {
    if (v < static_cast<u64>(k.lo))
        return false;
    return hi >= kKeyNoMax || v <= static_cast<u64>(hi);
}

bool in_range_i(const Key& k, i64 v, double hi) {
    if (v < static_cast<i64>(k.lo))
        return false;
    return hi >= 9223372036854775807.0 || v <= static_cast<i64>(hi);
}

} // namespace

// ---------------------------------------------------------------------------
// The public face
// ---------------------------------------------------------------------------

usize key_count() { return reg().keys.size(); }

const Key& key_at(usize i) { return reg().keys[i]; }

const Key* key_find(Str name) {
    const Registry& r = reg();
    const u64 h = hash_name(name);
    for (usize i = 0; i < r.keys.size(); ++i)
        if (r.hashes[i] == h && name.eq(r.keys[i].name))
            return &r.keys[i];
    return nullptr;
}

const char* key_kind_name(KeyKind k) {
    switch (k) {
    case KeyKind::flag:
        return "switch";
    case KeyKind::whole:
        return "whole";
    case KeyKind::number:
        return "number";
    case KeyKind::choice:
        return "choice";
    case KeyKind::text:
        return "text";
    }
    return "?";
}

KeyMisuseHook key_set_misuse_hook(KeyMisuseHook hook) {
    KeyMisuseHook prev = g_misuse;
    g_misuse = hook;
    return prev;
}

void key_set_trace_hook(KeyTraceHook hook) { g_trace = hook; }

void key_set_value_hook(KeyValueHook hook) { g_value = hook; }

void key_accept_known(Config& cfg) {
    for (usize i = 0; i < cfg.count(); ++i)
        if (key_find(cfg.key(i)))
            (void)cfg.get_str(cfg.key(i), Str()); // marks it used; reads nothing
}

bool key_flag(Config& cfg, Str name, KeyAt at) {
    const Key* k = entry(name, KeyKind::flag, at);
    if (!k)
        return false;
    const bool v = cfg.get_bool(name, def_of(*k, at) != 0);
    report(*k, v ? "true" : "false");
    return v;
}

u64 key_u64(Config& cfg, Str name, KeyAt at) {
    const Key* k = entry(name, KeyKind::whole, at);
    if (!k)
        return 0;
    if (k->lo < 0) {
        misuse(name, "can be negative: read it with key_i64");
        return 0;
    }
    const double hi = hi_of(*k, at);
    const u64 v = cfg.get_u64(name, static_cast<u64>(def_of(*k, at)));
    if (!in_range_u(*k, v, hi)) {
        char what[96];
        range_message_u(static_cast<u64>(k->lo), hi, what, sizeof what);
        cfg.fail(name, what);
        report_u(*k, static_cast<u64>(k->lo));
        return static_cast<u64>(k->lo);
    }
    report_u(*k, v);
    return v;
}

u32 key_u32(Config& cfg, Str name, KeyAt at) {
    const Key* k = key_find(name);
    if (k && k->kind == KeyKind::whole && k->hi > 4294967295.0) {
        misuse(name, "has a maximum over 32 bits: read it with key_u64");
        return 0;
    }
    return static_cast<u32>(key_u64(cfg, name, at));
}

i64 key_i64(Config& cfg, Str name, KeyAt at) {
    const Key* k = entry(name, KeyKind::whole, at);
    if (!k)
        return 0;
    const double hi = hi_of(*k, at);
    const i64 v = cfg.get_i64(name, static_cast<i64>(def_of(*k, at)));
    if (!in_range_i(*k, v, hi)) {
        char what[96];
        if (hi >= 9223372036854775807.0)
            snprintf(what, sizeof what, "must be %lld or more", static_cast<long long>(k->lo));
        else
            snprintf(what, sizeof what, "must be between %lld and %lld", static_cast<long long>(k->lo),
                     static_cast<long long>(hi));
        cfg.fail(name, what);
        report_i(*k, static_cast<i64>(k->lo));
        return static_cast<i64>(k->lo);
    }
    report_i(*k, v);
    return v;
}

double key_f64(Config& cfg, Str name, KeyAt at) {
    const Key* k = entry(name, KeyKind::number, at);
    if (!k)
        return 0;
    const double hi = hi_of(*k, at);
    const double v = cfg.get_f64(name, def_of(*k, at));
    if (!in_range_f(*k, v, hi)) {
        char what[96];
        range_message_f(*k, hi, what, sizeof what);
        cfg.fail(name, what);
        report_f(*k, k->lo);
        return k->lo;
    }
    report_f(*k, v);
    return v;
}

Str key_choice(Config& cfg, Str name) {
    const Key* k = entry(name, KeyKind::choice, KeyAt{});
    if (!k)
        return Str();
    const Str v = cfg.get_str(name, k->def_text);
    if (!has_option(k->options, v)) {
        char what[192];
        options_message(k->options, what, sizeof what);
        cfg.fail(name, what);
        report(*k, k->def_text);
        return Str(k->def_text);
    }
    report(*k, v);
    return v;
}

Str key_str(Config& cfg, Str name) {
    const Key* k = entry(name, KeyKind::text, KeyAt{});
    if (!k)
        return Str();
    const Str v = cfg.get_str(name, k->def_text);
    report(*k, v);
    return v;
}

Str key_text(Config& cfg, Str name) {
    const Key* k = key_find(name);
    if (!k) {
        misuse(name, "has no entry in the settings registry (add one to core/src/keys.inc)");
        return Str();
    }
    if (k->flags & (kKeyComputed | kKeyMaxComputed)) {
        misuse(name, "is computed: read it with its own reader");
        return Str();
    }
    if (g_trace)
        g_trace(*k);
    const Str v = cfg.get_str(name, k->def_text);
    report(*k, v);
    return v;
}

// Peeks: an estimate's look at a setting the world reads (and checks) later.
namespace {
const Key* peek_entry(Str name, KeyKind kind) {
    const Key* k = key_find(name);
    if (!k) {
        misuse(name, "has no entry in the settings registry (add one to core/src/keys.inc)");
        return nullptr;
    }
    const bool ok = k->kind == kind || (kind == KeyKind::text && k->kind == KeyKind::choice);
    if (!ok) {
        misuse(name, "is peeked with the wrong reader for its kind");
        return nullptr;
    }
    if (k->flags & kKeyComputed) {
        misuse(name, "has a computed default: it can't be peeked");
        return nullptr;
    }
    if (g_trace)
        g_trace(*k);
    return k;
}
} // namespace

bool key_peek_flag(Config& cfg, Str name) {
    const Key* k = peek_entry(name, KeyKind::flag);
    return k ? cfg.get_bool(name, k->def != 0) : false;
}

u64 key_peek_u64(Config& cfg, Str name) {
    const Key* k = peek_entry(name, KeyKind::whole);
    return k ? cfg.get_u64(name, static_cast<u64>(k->def)) : 0;
}

double key_peek_f64(Config& cfg, Str name) {
    const Key* k = peek_entry(name, KeyKind::number);
    return k ? cfg.get_f64(name, k->def) : 0;
}

Str key_peek_str(Config& cfg, Str name) {
    const Key* k = peek_entry(name, KeyKind::text);
    return k ? cfg.get_str(name, k->def_text) : Str();
}

u64 key_peek_u64_assuming(Config& cfg, Str name, u64 assume) {
    return peek_entry(name, KeyKind::whole) ? cfg.get_u64(name, assume) : 0;
}

bool key_given(Config& cfg, Str name) {
    if (!key_find(name)) {
        misuse(name, "has no entry in the settings registry (add one to core/src/keys.inc)");
        return false;
    }
    return cfg.get_str(name, "").n > 0;
}

bool key_check(Str name, Str text, KeyAt at, double* out) {
    const Key* k = key_find(name);
    if (!k) {
        misuse(name, "has no entry in the settings registry (add one to core/src/keys.inc)");
        return false;
    }
    if (((k->flags & kKeyMaxComputed) != 0) != (at.hi != kKeyUnset)) {
        misuse(name, "is checked without its computed maximum (or with one it doesn't have)");
        return false;
    }
    const double hi = hi_of(*k, at);
    switch (k->kind) {
    case KeyKind::flag: {
        bool b;
        if (!parse_bool(text, &b))
            return false;
        *out = b ? 1.0 : 0.0;
        return true;
    }
    case KeyKind::whole:
        if (k->lo < 0) {
            i64 v;
            if (!parse_i64(text, &v) || !in_range_i(*k, v, hi))
                return false;
            *out = static_cast<double>(v);
            return true;
        } else {
            u64 v;
            if (!parse_u64(text, &v) || !in_range_u(*k, v, hi))
                return false;
            *out = static_cast<double>(v);
            return true;
        }
    case KeyKind::number: {
        double v;
        if (!parse_f64(text, &v) || !in_range_f(*k, v, hi))
            return false;
        *out = v;
        return true;
    }
    case KeyKind::choice:
        return has_option(k->options, text);
    case KeyKind::text:
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// nova keys: the table and the Markdown reference
// ---------------------------------------------------------------------------

namespace {

void append_num(TextBuf& t, const Key& k, double v) {
    if (k.kind == KeyKind::whole) {
        if (v < 0)
            t.append_i64(static_cast<i64>(v));
        else
            t.append_u64(static_cast<u64>(v));
    } else {
        t.append_f64(v);
    }
}

// "0 to 1000000", "at least 1", "above 0, at most 1", "1 to 16777216 (max computed)" ...
void append_range(TextBuf& t, const Key& k) {
    switch (k.kind) {
    case KeyKind::flag:
        t.append("true or false");
        return;
    case KeyKind::choice: {
        const Str o(k.options);
        for (usize i = 0; i < o.n; ++i) {
            if (o.p[i] == ' ')
                t.append(", ");
            else
                t.append_char(o.p[i]);
        }
        return;
    }
    case KeyKind::text:
        t.append("text");
        return;
    case KeyKind::whole:
    case KeyKind::number:
        break;
    }
    const bool open = (k.flags & kKeyOpenMin) != 0;
    const bool unbounded = k.kind == KeyKind::whole && k.lo < 0 ? k.hi >= 9223372036854775807.0 : k.hi >= kKeyNoMax;
    if (unbounded) {
        t.append(open ? "above " : "at least ");
        append_num(t, k, k.lo);
        return;
    }
    if (open) {
        t.append("above ");
        append_num(t, k, k.lo);
        t.append(", at most ");
    } else {
        append_num(t, k, k.lo);
        t.append(" to ");
    }
    append_num(t, k, k.hi);
    if (k.flags & kKeyMaxComputed)
        t.append(" (max computed)");
}

void append_default(TextBuf& t, const Key& k) {
    t.append(k.def_text[0] ? Str(k.def_text) : Str("(empty)"));
    if (k.flags & kKeyComputed)
        t.append(" (computed)");
}

void pad(TextBuf& t, usize from, usize width) {
    while (t.size() - from < width)
        t.append_char(' ');
    t.append_char(' ');
}

bool group_has(const char* group, bool hidden) {
    for (usize i = 0; i < key_count(); ++i) {
        const Key& k = key_at(i);
        if (strcmp(k.group, group) == 0 && ((k.flags & kKeyHidden) != 0) == hidden)
            return true;
    }
    return false;
}

// Markdown cell text: '|' escaped, newlines flattened, and outside a code
// span '<' and '>' as entities (so "<tick>" isn't taken for an HTML tag).
void md_cell(TextBuf& t, Str s, bool code = false) {
    for (usize i = 0; i < s.n; ++i) {
        if (s.p[i] == '|')
            t.append("\\|");
        else if (!code && s.p[i] == '<')
            t.append("&lt;");
        else if (!code && s.p[i] == '>')
            t.append("&gt;");
        else if (s.p[i] == '\n' || s.p[i] == '\r')
            t.append_char(' ');
        else
            t.append_char(s.p[i]);
    }
}

// The anchor GitHub gives a heading: lower case, spaces to '-', the rest dropped.
void md_anchor(TextBuf& t, const char* heading) {
    for (const char* c = heading; *c; ++c) {
        if (*c >= 'A' && *c <= 'Z')
            t.append_char(static_cast<char>(*c - 'A' + 'a'));
        else if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')
            t.append_char(*c);
        else if (*c == ' ')
            t.append_char('-');
    }
}

// One table row per setting; a family with several members in this group is
// one row, its template, with the members listed.
void md_group(TextBuf& t, const char* group, bool hidden) {
    t.append("| Key | Kind | Default | Range | Setting |\n");
    t.append("|---|---|---|---|---|\n");
    const usize n = key_count();
    for (usize i = 0; i < n; ++i) {
        const Key& k = key_at(i);
        if (strcmp(k.group, group) != 0 || ((k.flags & kKeyHidden) != 0) != hidden)
            continue;
        // A family: its first member in this group stands for the others.
        usize members = 0;
        bool first = true;
        if (k.family) {
            for (usize j = 0; j < n; ++j) {
                const Key& o = key_at(j);
                if (o.family == k.family && strcmp(o.group, group) == 0 && ((o.flags & kKeyHidden) != 0) == hidden) {
                    if (j < i)
                        first = false;
                    ++members;
                }
            }
        }
        if (!first)
            continue;
        const bool as_family = members > 1;
        t.append("| `");
        md_cell(t, as_family ? Str(k.family) : Str(k.name), true);
        t.append("` | ");
        t.append(key_kind_name(k.kind));
        if (k.flags & kKeyLog)
            t.append(", log");
        t.append(" | ");
        TextBuf d;
        append_default(d, k);
        if (as_family) { // members with a default of their own
            for (usize j = 0; j < n; ++j) {
                const Key& o = key_at(j);
                if (o.family == k.family && strcmp(o.group, group) == 0 && strcmp(o.def_text, k.def_text) != 0) {
                    d.append("; ");
                    d.append(o.name);
                    d.append(": ");
                    d.append(o.def_text);
                }
            }
        }
        md_cell(t, d.view());
        t.append(" | ");
        TextBuf r;
        append_range(r, k);
        md_cell(t, r.view());
        t.append(" | **");
        md_cell(t, as_family ? Str(k.family_label) : Str(k.label));
        t.append("**. ");
        md_cell(t, as_family ? Str(k.family_help) : Str(k.help));
        if (as_family) {
            t.append(" For each of: ");
            bool comma = false;
            for (usize j = 0; j < n; ++j) {
                const Key& o = key_at(j);
                if (o.family != k.family || strcmp(o.group, group) != 0)
                    continue;
                if (comma)
                    t.append(", ");
                t.append_char('`');
                md_cell(t, o.name, true);
                t.append_char('`');
                comma = true;
            }
            t.append(".");
        }
        if (k.needs[0]) {
            t.append(" Needs `");
            md_cell(t, k.needs, true);
            t.append("`.");
        }
        t.append(" |\n");
    }
}

} // namespace

void keys_table(TextBuf& t) {
    const usize n = key_count();
    usize width = 0;
    for (usize i = 0; i < n; ++i) {
        const usize w = strlen(key_at(i).name);
        width = w > width ? w : width;
    }
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!group_has(kKeyGroups[g], false) && !group_has(kKeyGroups[g], true))
            continue;
        t.append("== ");
        t.append(kKeyGroups[g]);
        t.append(" ==\n");
        for (usize i = 0; i < n; ++i) {
            const Key& k = key_at(i);
            if (strcmp(k.group, kKeyGroups[g]) != 0)
                continue;
            const usize row = t.size();
            t.append(k.name);
            pad(t, row, width);
            usize at = t.size();
            t.append(key_kind_name(k.kind));
            if (k.flags & kKeyLog)
                t.append(",log");
            pad(t, at, 10);
            at = t.size();
            append_default(t, k);
            pad(t, at, 16);
            at = t.size();
            append_range(t, k);
            pad(t, at, 28);
            t.append(k.label);
            if (k.flags & kKeyHidden)
                t.append(" (hidden)");
            t.append(": ");
            t.append(k.help);
            if (k.needs[0]) {
                t.append(" [needs ");
                t.append(k.needs);
                t.append("]");
            }
            t.append_char('\n');
        }
    }
    u32 families = 0;
    for (usize i = 0; i < n; ++i)
        if (key_at(i).family && (i == 0 || key_at(i - 1).family != key_at(i).family))
            ++families;
    char line[96];
    snprintf(line, sizeof line, "%llu settings (%u families expanded)\n", static_cast<unsigned long long>(n),
             families);
    t.append(line);
}

void keys_markdown(TextBuf& t) {
    t.append("# Settings reference\n\n");
    t.append("Generated by `nova keys --md docs/keys.md` from the settings registry, `core/src/keys.inc` "
             "(docs/config-plan.md C1). Don't edit it by hand: change the registry and generate it again.\n\n");
    t.append("Every setting is a `key = value` line in a config file, or `--set key=value` on the command line. "
             "A setting left out takes its default. A key nothing reads is an error (a typo, most likely).\n\n");
    t.append("- **Kinds:** *switch* (true or false), *whole* (a whole number), *number*, *choice* (one of the "
             "options listed), *text*. *log* marks a rate spanning decades (a logarithmic knob on the page).\n");
    t.append("- **Computed:** a default or a maximum worked out from other settings, as the setting's text "
             "says. The default shown is the one with every other setting at its own default.\n");
    t.append("- **Families:** a key with a `<placeholder>` stands for one setting per member listed.\n");
    t.append("- **Needs:** the switch the setting depends on; it does nothing while that is off.\n\n");
    const usize n = key_count();
    u32 families = 0, hidden = 0;
    for (usize i = 0; i < n; ++i) {
        if (key_at(i).family && (i == 0 || key_at(i - 1).family != key_at(i).family))
            ++families;
        hidden += (key_at(i).flags & kKeyHidden) != 0;
    }
    char line[160];
    u32 members = 0;
    for (usize i = 0; i < n; ++i)
        members += key_at(i).family != nullptr;
    snprintf(line, sizeof line, "%llu settings, %u of them from %u families; %u hidden (tests and demos).\n\n",
             static_cast<unsigned long long>(n), members, families, hidden);
    t.append(line);
    t.append("## Contents\n\n");
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!group_has(kKeyGroups[g], false))
            continue;
        t.append("- [");
        t.append(kKeyGroups[g]);
        t.append("](#");
        md_anchor(t, kKeyGroups[g]);
        t.append(")\n");
    }
    t.append("- [Hidden settings](#hidden-settings)\n\n");
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!group_has(kKeyGroups[g], false))
            continue;
        t.append("## ");
        t.append(kKeyGroups[g]);
        t.append("\n\n");
        md_group(t, kKeyGroups[g], false);
        t.append("\n");
    }
    t.append("## Hidden settings\n\n");
    t.append("Settings for tests and the demo world, left off the setup page.\n\n");
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!group_has(kKeyGroups[g], true))
            continue;
        t.append("### ");
        t.append(kKeyGroups[g]);
        t.append("\n\n");
        md_group(t, kKeyGroups[g], true);
        t.append("\n");
    }
}

} // namespace nova
