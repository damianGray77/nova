#include "setup.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "nova/hash.h" // new_map's seeds
#include "nova/io.h"
#include "nova/keys.h" // the settings registry: everything on the page comes from it
#include "nova/pool.h" // clock_seconds
#include "nova/view.h" // the map's colours (the viewer's)
#include "theme.h"

namespace nova {

namespace {

// --- small helpers ------------------------------------------------------------------

i32 imin(i32 a, i32 b) { return a < b ? a : b; }
i32 imax(i32 a, i32 b) { return a > b ? a : b; }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Does `hay` contain `needle`, ignoring case?
bool icontains(Str hay, Str needle) {
    if (needle.n == 0)
        return true;
    for (usize i = 0; i + needle.n <= hay.n; ++i) {
        usize k = 0;
        while (k < needle.n && lower(hay.p[i + k]) == lower(needle.p[k]))
            ++k;
        if (k == needle.n)
            return true;
    }
    return false;
}

bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

// The part of a path after its last slash.
Str basename(Str p) {
    usize at = 0;
    for (usize i = 0; i < p.n; ++i)
        if (p.p[i] == '/' || p.p[i] == '\\')
            at = i + 1;
    return p.sub(at, p.n - at);
}

// The n-th line of a '\n'-separated list.
Str line_at(const char* lines, u32 n) {
    const Str s(lines);
    u32 k = 0;
    usize at = 0;
    for (usize i = 0; i <= s.n; ++i) {
        if (i == s.n || s.p[i] == '\n') {
            if (i > at) {
                if (k == n)
                    return s.sub(at, i - at);
                ++k;
            }
            at = i + 1;
        }
    }
    return Str();
}

u32 line_count(const char* lines) {
    u32 n = 0;
    while (line_at(lines, n).n)
        ++n;
    return n;
}

// The n-th of a choice's space-separated options.
Str option_at(const char* options, u32 n) {
    const Str s(options);
    u32 k = 0;
    usize at = 0;
    for (usize i = 0; i <= s.n; ++i) {
        if (i == s.n || s.p[i] == ' ') {
            if (i > at) {
                if (k == n)
                    return s.sub(at, i - at);
                ++k;
            }
            at = i + 1;
        }
    }
    return Str();
}

bool has_word(Str list, char sep, Str w) {
    usize at = 0;
    for (usize i = 0; i <= list.n; ++i) {
        if (i == list.n || list.p[i] == sep) {
            if (list.sub(at, i - at).eq(w))
                return true;
            at = i + 1;
        }
    }
    return false;
}

// Two values of a setting mean the same ("1e-3" and "0.001", "on" and "true").
bool same_value(const Key& k, Str a, Str b) {
    a = a.trim();
    b = b.trim();
    if (a.eq(b))
        return true;
    switch (k.kind) {
    case KeyKind::flag: {
        bool x = false, y = false;
        return parse_bool(a, &x) && parse_bool(b, &y) && x == y;
    }
    case KeyKind::whole: {
        u64 x = 0, y = 0;
        if (parse_u64(a, &x) && parse_u64(b, &y))
            return x == y;
        i64 p = 0, q = 0;
        return parse_i64(a, &p) && parse_i64(b, &q) && p == q;
    }
    case KeyKind::number: {
        double x = 0, y = 0;
        return parse_f64(a, &x) && parse_f64(b, &y) && x == y;
    }
    default:
        return false;
    }
}

double num_of(Str s) {
    double v = 0;
    if (parse_f64(s.trim(), &v))
        return v;
    u64 u = 0;
    if (parse_u64(s.trim(), &u))
        return static_cast<double>(u);
    return 0;
}

bool unbounded(const Key& k) { return k.hi >= 9.0e15; }

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

// "0 to 1", "at least 1", "above 0, at most 1", "ring, procedural, landscape" ...
void append_range(TextBuf& t, const Key& k) {
    switch (k.kind) {
    case KeyKind::flag:
        t.append("on or off");
        return;
    case KeyKind::choice:
        for (const char* o = k.options; *o; ++o)
            if (*o == ' ')
                t.append(", ");
            else
                t.append_char(*o);
        return;
    case KeyKind::text:
        t.append("text");
        return;
    default:
        break;
    }
    const bool open = (k.flags & kKeyOpenMin) != 0;
    if (unbounded(k)) {
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
        t.append(" (and at most what the node's size allows)");
}

// A slider's span: the setting's range, or for a very wide one the stretch of
// it about the default (or the loaded file's value) where values live -- the
// number field takes any value in range. Logarithmic where the registry says
// (a rate spanning decades), its left end the minimum itself (0: off). The
// span stays put while values move inside it; a value outside it re-centres
// it there.
struct Span {
    double lo = 0, hi = 1, a = 1; // a: the log scale's low end
    bool log = false, whole = false;
};

void make_span(const Key& k, double base, Span* s) {
    const double hi = k.hi;
    if (s->log) {
        s->a = s->lo > 0 ? s->lo : base * 1e-6;
        const double top = base * 1e6;
        s->hi = unbounded(k) || hi > top ? top : hi;
        if (s->hi <= s->a)
            s->hi = s->a * 10;
        return;
    }
    s->hi = hi;
    if (unbounded(k) || hi - s->lo > base * 20)
        s->hi = s->lo + base * 10 < hi ? s->lo + base * 10 : hi;
    if (s->hi <= s->lo)
        s->hi = s->lo + 1;
}

// cur: the value now; loaded: the loaded file's (or cur, when it has none).
Span span_of(const Key& k, double cur, double loaded) {
    Span s;
    s.lo = k.lo;
    s.whole = k.kind == KeyKind::whole;
    s.log = (k.flags & kKeyLog) != 0;
    if (s.log) {
        make_span(k, k.def > 0 ? k.def : (loaded > 0 ? loaded : 1.0), &s);
        if (cur > s.lo && (cur < s.a || cur > s.hi))
            make_span(k, cur, &s);
        return s;
    }
    const double d = k.def < 0 ? -k.def : k.def, l = loaded < 0 ? -loaded : loaded;
    double mag = d > l ? d : l;
    mag = mag > 1 ? mag : 1;
    make_span(k, mag, &s);
    if (cur > s.hi)
        make_span(k, cur < 0 ? -cur : cur, &s);
    return s;
}

double zone(const Span& s) { return s.log && s.lo <= 0 ? 0.03 : 0.0; }

double t_of(const Span& s, double v) {
    if (!s.log)
        return clampd((v - s.lo) / (s.hi - s.lo), 0, 1);
    const double z = zone(s);
    if (v <= s.lo)
        return 0;
    if (v <= s.a)
        return z;
    return clampd(z + (1 - z) * log(v / s.a) / log(s.hi / s.a), 0, 1);
}

double v_of(const Span& s, double t) {
    t = clampd(t, 0, 1);
    double v;
    if (!s.log) {
        v = s.lo + t * (s.hi - s.lo);
    } else {
        const double z = zone(s);
        if (z > 0 && t < z)
            return s.lo;
        v = s.a * exp((t - z) / (1 - z) * log(s.hi / s.a));
    }
    if (s.whole)
        v = floor(v + 0.5);
    return clampd(v, s.lo, s.hi);
}

// A slider's value as config text: whole numbers exact, numbers to 4 figures
// (3 on a log scale).
void slider_text(const Span& s, double v, char* out, usize n) {
    if (s.whole) {
        if (v < 0)
            snprintf(out, n, "%lld", static_cast<long long>(v));
        else
            snprintf(out, n, "%llu", static_cast<unsigned long long>(v));
    } else {
        snprintf(out, n, s.log ? "%.3g" : "%.4g", v);
    }
}

// Word wrap of proportional text into lines no wider than w; emit(start, len).
template <class Emit> i32 wrap_text(Canvas& c, const char* s, i32 w, u32 font, Emit emit) {
    const usize n = strlen(s);
    usize at = 0;
    i32 lines = 0;
    while (at < n) {
        usize end = at, last_space = at;
        while (end < n && s[end] != '\n') {
            if (s[end] == ' ')
                last_space = end;
            if (c.text_w(s + at, end - at + 1, font) > w && end > at) {
                if (last_space > at)
                    end = last_space;
                break;
            }
            ++end;
        }
        emit(at, end - at);
        ++lines;
        at = end;
        while (at < n && (s[at] == ' ' || s[at] == '\n'))
            ++at;
    }
    return lines;
}

// "1.2 GB", "340 MB"
void bytes_text(double b, char* out, usize n) {
    if (b >= 1073741824.0)
        snprintf(out, n, "%.1f GB", b / 1073741824.0);
    else if (b >= 1048576.0)
        snprintf(out, n, "%.0f MB", b / 1048576.0);
    else
        snprintf(out, n, "%.0f KB", b / 1024.0);
}

u32 group_of(const Key& k) {
    for (u32 g = 0; g < kKeyGroupCount; ++g)
        if (strcmp(k.group, kKeyGroups[g]) == 0)
            return g;
    return 0;
}

bool group_shown(u32 g) {
    for (usize i = 0; i < key_count(); ++i)
        if (!(key_at(i).flags & kKeyHidden) && group_of(key_at(i)) == g)
            return true;
    return false;
}

} // namespace

// --- the settings ---------------------------------------------------------------------

SetupPage::SetupPage() {
    cur_.resize(key_count());
    file_.resize(key_count());
    row_r_.resize(key_count());
    ctl_r_.resize(key_count());
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (group_shown(g)) {
            group_ = g;
            break;
        }
    }
    snprintf(source_, sizeof source_, "defaults");
}

usize SetupPage::key_index(Str name) const {
    const Key* k = key_find(name);
    return k ? static_cast<usize>(k - &key_at(0)) : kNoKey;
}

void SetupPage::store(Array<Val>& vals, usize k, Str v) {
    // (v may point into text_ itself: copy it out before text_ can move)
    TextBuf copy;
    copy.append(v);
    Val& x = vals[k];
    x.off = static_cast<u32>(text_.size());
    x.len = static_cast<u32>(copy.size());
    x.set = 1;
    text_.append(copy.view().p, copy.size());
    redraw_ = true;
}

Str SetupPage::value(usize k) const {
    const Val& v = cur_[k];
    return v.set ? Str(text_.data() + v.off, v.len) : Str();
}

Str SetupPage::file_value(usize k) const {
    const Val& v = file_[k];
    return v.set ? Str(text_.data() + v.off, v.len) : Str();
}

bool SetupPage::load_text(Str text, const char* source, TextBuf* err) {
    Config c;
    Config::ParseError pe;
    if (!c.parse(text, &pe)) {
        err->append(pe.msg);
        return false;
    }
    text_.clear();
    unknown_.clear();
    for (usize i = 0; i < cur_.size(); ++i) {
        cur_[i] = Val{};
        file_[i] = Val{};
    }
    for (usize i = 0; i < c.count(); ++i) {
        const usize k = key_index(c.key(i));
        if (k == kNoKey) {
            unknown_.append(c.key(i).p, c.key(i).n);
            unknown_.append(" = ", 3);
            unknown_.append(c.value(i).p, c.value(i).n);
            unknown_.push('\n');
            continue;
        }
        store(cur_, k, c.value(i));
        file_[k] = cur_[k];
    }
    snprintf(source_, sizeof source_, "%s", source);
    dirty_ = true;
    scroll_ = 0;
    edit_ = kEditNone;
    drag_ = kNoKey;
    popup_ = kPopNone;
    make_folder_name();
    return true;
}

bool SetupPage::load_file(const char* path, TextBuf* err) {
    Array<u8> bytes;
    if (!read_file(path, &bytes)) {
        err->append("cannot read '");
        err->append(path);
        err->append("'");
        return false;
    }
    TextBuf why;
    if (!load_text(Str(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, &why)) {
        err->append(path);
        err->append(": ");
        err->append(why.view());
        return false;
    }
    return true;
}

void SetupPage::load_defaults(const char* source) {
    TextBuf err;
    load_text(Str(), source, &err);
}

void SetupPage::set_mode(u32 mode, const char* title) {
    mode_ = mode;
    snprintf(title_, sizeof title_, "%s", title ? title : "");
    edit_ = kEditNone;
    drag_ = kNoKey;
    popup_ = kPopNone;
    dirty_ = true;
}

void SetupPage::set_info(const char* text) { snprintf(info_, sizeof info_, "%s", text ? text : ""); }

void SetupPage::set_presets(Str lines) {
    const usize n = lines.n < sizeof presets_ - 1 ? lines.n : sizeof presets_ - 1;
    memcpy(presets_, lines.p, n);
    presets_[n] = '\0';
}

void SetupPage::set_run_folders(Str lines) {
    const usize n = lines.n < sizeof folders_ - 1 ? lines.n : sizeof folders_ - 1;
    memcpy(folders_, lines.p, n);
    folders_[n] = '\0';
}

void SetupPage::set_note(const char* text, bool bad) {
    snprintf(note_, sizeof note_, "%s", text ? text : "");
    note_bad_ = bad;
}

void SetupPage::set_headless(const char* folder) { snprintf(headless_, sizeof headless_, "%s", folder ? folder : ""); }

void SetupPage::new_map() {
    const usize k = key_index("world.seed");
    if (k == kNoKey || read_only())
        return;
    // A fresh number from the clock, this page's address and a count (the
    // map is then the run's: world.seed goes into run.cfg like any setting).
    const double t = clock_seconds();
    u64 bits = 0;
    memcpy(&bits, &t, sizeof bits);
    const u64 z = mix64(bits ^ mix64(reinterpret_cast<u64>(this) + kGolden * ++new_maps_));
    char b[24];
    snprintf(b, sizeof b, "%llu", static_cast<unsigned long long>(z % 1000000000ull));
    TextBuf why;
    set_text(k, Str(b), &why);
    TextBuf note;
    note.append("new map: world.seed = ");
    note.append(b);
    set_note(note.c_str());
}

Str SetupPage::effective(usize k) {
    if (cur_[k].set)
        return value(k);
    const SettingsCheck& ch = check();
    if (ch.read.size() > k && ch.read[k])
        return ch.effective(k);
    return Str(key_at(k).def_text);
}

Str SetupPage::shown(usize k) { return effective(k); }

bool SetupPage::set_text(usize k, Str text, TextBuf* why) {
    const Key& key = key_at(k);
    Str v = text.trim();
    if (v.find('#') < v.n || v.find('\n') < v.n) {
        why->append("a value can't hold '#' or a line break");
        return false;
    }
    TextBuf held;
    switch (key.kind) {
    case KeyKind::flag: {
        bool b = false;
        if (!parse_bool(v, &b)) {
            why->append("on or off (true, false)");
            return false;
        }
        v = b ? "true" : "false";
        break;
    }
    case KeyKind::choice:
        if (!has_word(Str(key.options), ' ', v)) {
            why->append("must be one of: ");
            append_range(*why, key);
            return false;
        }
        break;
    case KeyKind::text:
        break;
    case KeyKind::whole:
    case KeyKind::number: {
        double d = 0;
        bool ok;
        if (key.kind == KeyKind::whole) {
            u64 u = 0;
            i64 s = 0;
            ok = key.lo < 0 ? parse_i64(v, &s) : parse_u64(v, &u);
            d = key.lo < 0 ? static_cast<double>(s) : static_cast<double>(u);
        } else {
            ok = parse_f64(v, &d);
        }
        if (!ok) {
            why->append(key.kind == KeyKind::whole ? "a whole number" : "a number");
            return false;
        }
        const bool open = (key.flags & kKeyOpenMin) != 0;
        if (open && d <= key.lo) {
            why->append("must be ");
            append_range(*why, key);
            return false;
        }
        // Held to its range (a whole number's text kept exact when in range).
        if (d < key.lo || (!unbounded(key) && d > key.hi)) {
            append_num(held, key, d < key.lo ? key.lo : key.hi);
            v = held.view();
        }
        break;
    }
    }
    store(cur_, k, v);
    dirty_ = true;
    return true;
}

void SetupPage::unset(usize k) {
    cur_[k].set = 0;
    dirty_ = redraw_ = true;
}

void SetupPage::reset(usize k) {
    cur_[k] = file_[k];
    dirty_ = redraw_ = true;
}

bool SetupPage::changed(usize k) const {
    const Val& a = cur_[k];
    const Val& b = file_[k];
    if (!a.set && !b.set)
        return false;
    const Key& key = key_at(k);
    if (a.set != b.set && (key.flags & kKeyComputed))
        return true; // set against worked out: not the same thing
    const Str x = a.set ? value(k) : Str(key.def_text);
    const Str y = b.set ? file_value(k) : Str(key.def_text);
    return !same_value(key, x, y);
}

usize SetupPage::changed_count() const {
    usize n = 0;
    for (usize k = 0; k < cur_.size(); ++k)
        n += changed(k);
    return n;
}

bool SetupPage::needs_met(usize k, u32 depth) {
    const Str needs(key_at(k).needs);
    if (needs.n == 0 || depth > 8)
        return true;
    const usize eq = needs.find('=');
    const Str name = eq < needs.n ? needs.sub(0, eq) : needs;
    const usize j = key_index(name);
    if (j == kNoKey)
        return true;
    if (!needs_met(j, depth + 1))
        return false; // its switch's own switch is off
    const Str v = effective(j).trim();
    if (eq < needs.n)
        return has_word(needs.sub(eq + 1, needs.n - eq - 1), '|', v);
    const Key& nk = key_at(j);
    switch (nk.kind) {
    case KeyKind::flag: {
        bool b = false;
        return parse_bool(v, &b) && b;
    }
    case KeyKind::whole:
    case KeyKind::number:
        return num_of(v) != 0;
    default:
        return v.n > 0 && !v.eq("off");
    }
}

bool SetupPage::greyed(usize k) { return !needs_met(k, 0); }

bool SetupPage::matches(usize k) const {
    const Key& key = key_at(k);
    // Every word of the search in the name, the label or the help.
    const Str s(search_);
    usize at = 0;
    for (usize i = 0; i <= s.n; ++i) {
        if (i == s.n || s.p[i] == ' ') {
            const Str w = s.sub(at, i - at);
            if (w.n && !icontains(key.name, w) && !icontains(key.label, w) && !icontains(key.help, w))
                return false;
            at = i + 1;
        }
    }
    return true;
}

// --- files ------------------------------------------------------------------------------

void SetupPage::config_text(TextBuf& out) const {
    for (usize k = 0; k < cur_.size(); ++k) {
        if (!cur_[k].set)
            continue;
        out.append(key_at(k).name);
        out.append(" = ");
        out.append(value(k));
        out.append_char('\n');
    }
    out.append(Str(unknown_.data(), unknown_.size()));
}

namespace {

// "key = value" padded, then "# comment".
void cfg_line(TextBuf& out, const char* key, Str value, const char* comment) {
    const usize start = out.size();
    out.append(key);
    out.append(" = ");
    out.append(value);
    if (comment && comment[0]) {
        while (out.size() - start < 40)
            out.append_char(' ');
        out.append(" # ");
        out.append(comment);
    }
    out.append_char('\n');
}

} // namespace

void SetupPage::save_text(TextBuf& out) const {
    out.append("# Nova settings, saved from nova view's setup page (docs/config-plan.md C5).\n");
    out.append("# Loaded from: ");
    out.append(source_);
    out.append("\n# Only the settings that differ from their defaults are here; `nova keys` lists every\n"
               "# setting with its default, range and help. Run it with: nova run <this file>\n");
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        bool head = false;
        for (usize k = 0; k < cur_.size(); ++k) {
            const Key& key = key_at(k);
            if (!cur_[k].set || group_of(key) != g)
                continue;
            if (!(key.flags & kKeyComputed) && same_value(key, value(k), key.def_text))
                continue;
            if (!head) {
                out.append("\n# --- ");
                out.append(kKeyGroups[g]);
                out.append(" ---\n");
                head = true;
            }
            char comment[200];
            if (key.flags & kKeyComputed)
                snprintf(comment, sizeof comment, "%s (default worked out by the run)", key.label);
            else
                snprintf(comment, sizeof comment, "%s (default %s)", key.label, key.def_text[0] ? key.def_text : "empty");
            cfg_line(out, key.name, value(k), comment);
        }
    }
    if (unknown_.size()) {
        out.append("\n# Not settings (the file had them; nova run would refuse them):\n");
        const Str u(unknown_.data(), unknown_.size());
        usize at = 0;
        for (usize i = 0; i < u.n; ++i) {
            if (u.p[i] != '\n')
                continue;
            out.append("# ");
            out.append(u.sub(at, i - at + 1));
            at = i + 1;
        }
    }
}

bool SetupPage::save_file(const char* path, TextBuf* err) {
    TextBuf t;
    save_text(t);
    if (!write_file(path, t.view().p, t.size())) {
        err->append("cannot write '");
        err->append(path);
        err->append("' (does its folder exist?)");
        return false;
    }
    return true;
}

void SetupPage::run_cfg_text(const char* folder, TextBuf& out) {
    const SettingsCheck& ch = check();
    out.append("# Nova run settings, written by nova view's setup page when the run started\n"
               "# (docs/config-plan.md C3): every setting, defaults included, so this file alone\n"
               "# reproduces the run: nova run <this file> builds exactly the same world.\n");
    out.append("# Started from: ");
    out.append(source_);
    out.append_char('\n');
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        bool head = false;
        for (usize k = 0; k < cur_.size(); ++k) {
            const Key& key = key_at(k);
            if (group_of(key) != g)
                continue;
            const bool set = cur_[k].set != 0;
            const bool read = ch.read.size() > k && ch.read[k];
            if ((key.flags & kKeyHidden) && !set)
                continue; // tests' and records' settings: only when given
            if (!head) {
                out.append("\n# --- ");
                out.append(kKeyGroups[g]);
                out.append(" ---\n");
                head = true;
            }
            if ((key.flags & kKeyComputed) && !set && !read) {
                // Not read by the check: left for the run to work out, as nova run
                // would on the loaded file.
                out.append("# ");
                cfg_line(out, key.name, Str(key.def_text), "worked out by the run");
                continue;
            }
            TextBuf v;
            v.append(set ? value(k) : (read ? ch.effective(k) : Str(key.def_text)));
            // Outputs into the run's folder: each path that is on keeps its file name.
            if (outputs_here_ && folder && folder[0] && key.kind == KeyKind::text && Str(key.name).starts_with("output.") &&
                v.size()) {
                TextBuf p;
                p.append(folder);
                p.append_char('\\');
                p.append(basename(v.view()));
                v.clear();
                v.append(p.view());
            }
            char comment[120] = "";
            if ((key.flags & kKeyComputed) && !set)
                snprintf(comment, sizeof comment, "worked out from the other settings");
            else if (!same_value(key, v.view(), key.def_text))
                snprintf(comment, sizeof comment, "default %s", key.def_text[0] ? key.def_text : "empty");
            cfg_line(out, key.name, v.view(), comment);
        }
    }
}

// --- the check and Start ---------------------------------------------------------------

const SettingsCheck& SetupPage::check() {
    if (dirty_) {
        dirty_ = false; // (first: effective() asks for the check)
        TextBuf t;
        config_text(t);
        check_settings(t.view(), &check_, front_end_, machine_ && mode_ == kSetupEdit);
    }
    return check_;
}

usize SetupPage::problem_count() { return check().problems.size(); }

void SetupPage::set_folder(const char* f) { snprintf(folder_, sizeof folder_, "%s", f ? f : ""); }

void SetupPage::make_folder_name() {
    // The loaded file's name, without its folder, .cfg (or a copy's mark); a
    // run's own run.cfg goes by its folder's name.
    Str src(source_);
    if (src.n > 7 && memcmp(src.p + src.n - 7, " (copy)", 7) == 0)
        src = src.sub(0, src.n - 7);
    Str b = basename(src);
    if (b.eq("run.cfg") && src.n > b.n + 1)
        b = basename(src.sub(0, src.n - b.n - 1));
    if (b.n > 4 && memcmp(b.p + b.n - 4, ".cfg", 4) == 0)
        b = b.sub(0, b.n - 4);
    char name[200];
    bool ok = b.n > 0 && b.n < 120;
    for (usize i = 0; i < b.n && ok; ++i) {
        const char c = b.p[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
             c == '.';
    }
    snprintf(name, sizeof name, "%.*s", ok ? static_cast<int>(b.n) : 3, ok ? b.p : "run");
    for (u32 n = 1; n < 1000; ++n) {
        char f[260], probe[300];
        if (n == 1)
            snprintf(f, sizeof f, "runs\\%s", name);
        else
            snprintf(f, sizeof f, "runs\\%s-%u", name, n);
        snprintf(probe, sizeof probe, "%s\\run.cfg", f);
        if (!file_exists(probe)) {
            set_folder(f);
            return;
        }
    }
}

void SetupPage::default_folder() { make_folder_name(); }

// configs\<name>-2.cfg (the first free): a name beside the loaded file's, not over it.
void SetupPage::suggest_save_path() {
    Str b = basename(Str(read_only() && title_[0] ? title_ : source_));
    if (b.n > 4 && memcmp(b.p + b.n - 4, ".cfg", 4) == 0)
        b = b.sub(0, b.n - 4);
    bool ok = b.n > 0;
    for (usize i = 0; i < b.n && ok; ++i)
        ok = b.p[i] != ' ' && b.p[i] != '(' && b.p[i] != ':';
    for (u32 n = 2; n < 100; ++n) {
        snprintf(save_path_, sizeof save_path_, "configs\\%.*s-%u.cfg", ok ? static_cast<int>(b.n < 80 ? b.n : 80) : 8,
                 ok ? b.p : "settings", n);
        if (!file_exists(save_path_))
            break;
    }
}

const char* SetupPage::start_blocked() {
    if (mode_ != kSetupEdit)
        return "read-only";
    const usize p = problem_count();
    static char why[96];
    if (p) {
        snprintf(why, sizeof why, "%u problem%s to fix first", static_cast<unsigned>(p), p == 1 ? "" : "s");
        return why;
    }
    if (!folder_[0])
        return "pick a folder for the run";
    char probe[300];
    snprintf(probe, sizeof probe, "%s\\run.cfg", folder_);
    if (file_exists(probe))
        return "that folder holds a run already";
    return "";
}

bool SetupPage::can_start() { return start_blocked()[0] == '\0'; }

// --- test hooks --------------------------------------------------------------------------

void SetupPage::set_search(const char* text) {
    snprintf(search_, sizeof search_, "%s", text ? text : "");
    scroll_ = 0;
}

void SetupPage::set_changed_only(bool on) {
    changed_only_ = on;
    scroll_ = 0;
}

void SetupPage::select_group(u32 g) {
    group_ = g < kKeyGroupCount ? g : 0;
    search_[0] = '\0';
    changed_only_ = false;
    scroll_ = 0;
}

bool SetupPage::select_group_named(Str name) {
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        const Str gn(kKeyGroups[g]);
        bool same = gn.n == name.n;
        for (usize i = 0; i < gn.n && same; ++i)
            same = lower(gn.p[i]) == lower(name.p[i]);
        if (same) {
            select_group(g);
            return true;
        }
    }
    return false;
}

void SetupPage::show_first_problem() {
    const SettingsCheck& ch = check();
    for (usize i = 0; i < ch.problems.size(); ++i) {
        const usize k = key_index(ch.problems[i].key);
        if (k != kNoKey) {
            jump_to(k);
            return;
        }
    }
}

bool SetupPage::row_rect(usize k, Rect* r) const {
    if (k >= row_r_.size() || row_r_[k].w <= 0)
        return false;
    *r = row_r_[k];
    return true;
}

bool SetupPage::control_rect(usize k, Rect* r) const {
    if (k >= ctl_r_.size() || ctl_r_[k].w <= 0)
        return false;
    *r = ctl_r_[k];
    return true;
}

void SetupPage::jump_to(usize k) {
    group_ = group_of(key_at(k));
    search_[0] = '\0';
    changed_only_ = false;
    jump_ = k;
}

// --- widgets ------------------------------------------------------------------------------

u32 SetupPage::off_col() const { return ro_row_ ? kDim : kFaint; }

void SetupPage::set_tip(const char* text) {
    snprintf(tip_, sizeof tip_, "%s", text);
    tip_x_ = mx_;
    tip_y_ = my_;
}

bool SetupPage::button(const UiInput& in, Canvas& c, const Rect& r, const char* label, bool on, bool enabled) {
    const bool hot = enabled && r.contains(in.mx, in.my);
    c.fill(r, on ? kButtonOn : (hot ? kButtonHot : kButton));
    c.text(r.x + r.w / 2, r.y + (r.h - c.line_h(kFontUi)) / 2, label, kFontUi, enabled ? kText : kFaint, kAlignCenter,
           r.w - 4);
    if (hot)
        cursor_ = 3;
    return hot && in.pressed;
}

bool SetupPage::checkbox(const UiInput& in, Canvas& c, const Rect& r, bool on, bool enabled, const char* label) {
    const i32 s = c.px(16);
    const Rect box{r.x, r.y + (r.h - s) / 2, s, s};
    const bool hot = enabled && r.contains(in.mx, in.my);
    c.fill(box, enabled ? (hot ? kButtonHot : kField) : kPanel);
    c.frame(box, enabled ? (hot ? kAccent : kDim) : kFaint);
    if (on) { // a tick
        const u32 col = enabled ? kAccent : off_col();
        const i32 w = c.px(2) > 1 ? c.px(2) : 2;
        c.line(box.x + s / 5, box.y + s / 2, box.x + s * 2 / 5, box.y + s * 3 / 4, col, w);
        c.line(box.x + s * 2 / 5, box.y + s * 3 / 4, box.x + s * 4 / 5, box.y + s / 4, col, w);
    }
    if (label && label[0])
        c.text(box.right() + c.px(6), r.y + (r.h - c.line_h(kFontUi)) / 2, label, kFontUi,
               enabled ? kText : off_col());
    if (hot)
        cursor_ = 3;
    return hot && in.pressed;
}

void SetupPage::panel_box(Canvas& c, const Rect& r, const char* title, Rect* inner) {
    c.fill(r, kPanel);
    const i32 hh = c.line_h(kFontBold) + c.px(6);
    c.fill(Rect{r.x, r.y, r.w, hh}, kHead);
    c.text(r.x + c.px(8), r.y + c.px(3), title, kFontBold, kText, kAlignLeft, r.w - c.px(16));
    const i32 pad = c.px(6);
    *inner = Rect{r.x + pad, r.y + hh + pad, r.w - 2 * pad, r.h - hh - 2 * pad};
}

void SetupPage::begin_edit(u32 kind, usize key, Str text) {
    if (edit_ != kEditNone && (edit_ != kind || edit_key_ != key))
        commit_edit();
    edit_ = kind;
    edit_key_ = key;
    edit_bad_ = false;
    const usize n = text.n < sizeof edit_buf_ - 1 ? text.n : sizeof edit_buf_ - 1;
    memcpy(edit_buf_, text.p, n);
    edit_len_ = static_cast<u32>(n);
    edit_buf_[edit_len_] = '\0';
    edit_orig_.clear();
    edit_orig_.append(Str(edit_buf_, edit_len_));
    edit_r_ = Rect{};
}

void SetupPage::commit_edit() {
    const u32 kind = edit_;
    edit_buf_[edit_len_] = '\0';
    edit_ = kEditNone;
    switch (kind) {
    case kEditValue: {
        if (edit_key_ >= cur_.size() || read_only())
            break;
        if (Str(edit_buf_).trim().eq(edit_orig_.view().trim()) ||
            same_value(key_at(edit_key_), Str(edit_buf_), shown(edit_key_)))
            break; // nothing typed that changes it (an unset setting stays unset)
        TextBuf why;
        if (!set_text(edit_key_, Str(edit_buf_), &why)) {
            TextBuf t;
            t.append(key_at(edit_key_).name);
            t.append(": ");
            t.append(why.view());
            set_note(t.c_str(), true);
            edit_ = kEditValue; // keep it open, marked
            edit_bad_ = true;
        } else {
            set_note("");
        }
        break;
    }
    case kEditSearch:
        snprintf(search_, sizeof search_, "%s", edit_buf_);
        break;
    case kEditFolder:
        set_folder(Str(edit_buf_).trim().n ? edit_buf_ : "");
        break;
    case kEditSave: {
        snprintf(save_path_, sizeof save_path_, "%s", edit_buf_);
        TextBuf err;
        if (save_file(save_path_, &err)) {
            char t[320];
            snprintf(t, sizeof t, "saved %s (the settings that differ from the defaults)", save_path_);
            set_note(t);
        } else {
            set_note(err.c_str(), true);
        }
        break;
    }
    case kEditLoad: {
        snprintf(load_path_, sizeof load_path_, "%s", edit_buf_);
        TextBuf err;
        if (load_file(load_path_, &err)) {
            set_note("");
        } else {
            set_note(err.c_str(), true);
        }
        break;
    }
    default:
        break;
    }
}

void SetupPage::type_keys(const UiInput& in) {
    if (edit_ == kEditNone)
        return;
    for (u32 i = 0; i < in.nchars; ++i) {
        const char ch = in.chars[i];
        if (ch >= 32 && ch < 127 && ch != '#' && edit_len_ + 1 < sizeof edit_buf_)
            edit_buf_[edit_len_++] = ch;
    }
    for (u32 i = 0; i < in.nkeys && edit_ != kEditNone; ++i) {
        if (in.keys[i] == kVkBack && edit_len_ > 0) {
            --edit_len_;
        } else if (in.keys[i] == kVkReturn) {
            commit_edit();
        } else if (in.keys[i] == kVkEscape) {
            if (edit_ == kEditSearch) {
                edit_len_ = 0;
                search_[0] = '\0';
            }
            edit_ = kEditNone;
            edit_bad_ = false;
        }
    }
    edit_buf_[edit_len_] = '\0';
    if (edit_ == kEditSearch) { // the search applies as it is typed
        if (strcmp(search_, edit_buf_) != 0)
            scroll_ = 0;
        snprintf(search_, sizeof search_, "%s", edit_buf_);
    }
}

void SetupPage::field(const UiInput& in, Canvas& c, const Rect& r, u32 kind, usize key, Str text, bool enabled,
                      u32 color, const char* hint) {
    const bool mine = edit_ == kind && edit_key_ == key;
    const bool hot = enabled && r.contains(in.mx, in.my);
    c.fill(r, mine ? kField : (enabled ? kBg : kPanel));
    c.frame(r, mine ? (edit_bad_ ? kBad : kAccent) : (hot ? kDim : kFaint));
    const i32 ty = r.y + (r.h - c.line_h(kFontMono)) / 2;
    const i32 pad = c.px(4);
    if (mine) {
        edit_r_ = r;
        char shown_text[2064];
        snprintf(shown_text, sizeof shown_text, "%s|", edit_buf_);
        const usize n = strlen(shown_text);
        // Show the end of a long value: the caret stays in view.
        usize from = 0;
        while (from < n && c.text_w(shown_text + from, n - from, kFontMono) > r.w - 2 * pad)
            ++from;
        c.text(r.x + pad, ty, shown_text + from, n - from, kFontMono, kText, kAlignLeft, r.w - 2 * pad);
    } else if (text.n) {
        c.text(r.x + pad, ty, text.p, text.n, kFontMono, enabled ? color : off_col(), kAlignLeft, r.w - 2 * pad);
    } else if (hint[0]) {
        c.text(r.x + pad, r.y + (r.h - c.line_h(kFontSmall)) / 2, hint, kFontSmall, kFaint, kAlignLeft,
               r.w - 2 * pad);
    }
    if (hot) {
        cursor_ = 4;
        if (in.pressed && !mine)
            begin_edit(kind, key, text);
        if (in.pressed && !mine)
            edit_r_ = r;
    }
}

void SetupPage::slider(const UiInput& in, Canvas& c, usize k, const Rect& track, bool enabled) {
    const Key& key = key_at(k);
    const bool mine = drag_ == k;
    double v;
    Span s;
    if (mine) {
        s.lo = drag_lo_;
        s.hi = drag_hi_;
        s.a = drag_a_;
        s.log = drag_log_;
        s.whole = key.kind == KeyKind::whole;
        v = num_of(Str(drag_text_));
    } else {
        v = num_of(shown(k));
        s = span_of(key, v, file_[k].set ? num_of(file_value(k)) : ((key.flags & kKeyComputed) ? v : key.def));
    }
    const Rect hit{track.x - c.px(4), track.y - c.px(8), track.w + c.px(8), track.h + c.px(16)};
    const bool hot = enabled && hit.contains(in.mx, in.my);
    const i32 th = c.px(4);
    const Rect bar{track.x, track.y + track.h / 2 - th / 2, track.w, th};
    c.fill(bar, enabled ? kButton : kHead);
    const double t = t_of(s, v);
    const i32 hx = track.x + static_cast<i32>(t * track.w + 0.5);
    c.fill(Rect{bar.x, bar.y, hx - bar.x, bar.h}, enabled ? (mine ? kAccent : 0x3D5F8F) : (ro_row_ ? 0x34465F : kFaint));
    const i32 kw = c.px(8), kh = c.px(16);
    c.fill(Rect{hx - kw / 2, track.y + track.h / 2 - kh / 2, kw, kh},
           enabled ? (hot || mine ? kAccent : kDim) : off_col());
    if (hot)
        cursor_ = 3;
    if (hot && in.pressed && !mine) {
        drag_ = k;
        drag_lo_ = s.lo;
        drag_hi_ = s.hi;
        drag_a_ = s.a;
        drag_log_ = s.log;
        snprintf(drag_text_, sizeof drag_text_, "%.*s", static_cast<int>(shown(k).n), shown(k).p);
    }
    if (drag_ == k) {
        if (in.down || in.pressed) {
            Span ds = s;
            if (!mine) {
                ds.lo = drag_lo_;
                ds.hi = drag_hi_;
                ds.a = drag_a_;
                ds.log = drag_log_;
            }
            const double f = static_cast<double>(in.mx - track.x) / (track.w > 0 ? track.w : 1);
            slider_text(ds, v_of(ds, f), drag_text_, sizeof drag_text_);
            cursor_ = 3;
        }
        if (!in.down) { // released: the value applies
            TextBuf why;
            set_text(k, Str(drag_text_), &why);
            drag_ = kNoKey;
        }
    }
}

void SetupPage::open_popup(u32 kind, usize key, const Rect& anchor) {
    popup_ = kind;
    popup_key_ = key;
    popup_scroll_ = 0;
    popup_bar_.release();
    popup_r_ = Rect{anchor.x, anchor.bottom(), anchor.w, 0};
}

// --- the frame ----------------------------------------------------------------------------

void SetupPage::layout(Canvas& c) {
    const i32 W = c.width(), H = c.height();
    const i32 gap = c.px(4);
    const i32 barh = c.px(70);
    bar_ = Rect{0, 0, W, barh};
    const i32 gw = imin(c.px(220), W / 5);
    const i32 cw = imin(c.px(350), W / 4);
    groups_r_ = Rect{gap, barh + gap, gw, H - barh - 2 * gap};
    check_r_ = Rect{W - cw - gap, barh + gap, cw, H - barh - 2 * gap};
    list_r_ = Rect{groups_r_.right() + gap, barh + gap, check_r_.x - groups_r_.right() - 2 * gap, H - barh - 2 * gap};
}

void SetupPage::frame(const UiInput& input, Canvas& c) {
    UiInput in = input;
    mx_ = in.mx;
    my_ = in.my;
    tip_[0] = '\0';
    cursor_ = 0;
    redraw_ = false;
    for (usize k = 0; k < row_r_.size(); ++k) {
        row_r_[k] = Rect{};
        ctl_r_[k] = Rect{};
    }
    if (read_only() && edit_ != kEditNone && edit_ != kEditSearch && edit_ != kEditSave) {
        edit_ = kEditNone;
    }
    if (drag_ == kNoKey)
        check(); // a change made last frame: its check, before drawing
    layout(c);
    c.unclip();
    c.fill(Rect{0, 0, c.width(), c.height()}, kBg);

    type_keys(in);
    bool consumed = false;
    popup(in, c, &consumed); // input only; drawn last
    if (consumed) {
        in.pressed = in.released = in.dbl = false;
        in.wheel = 0;
    }
    const u32 edit_before = edit_;
    const usize edit_key_before = edit_key_;
    bar(in, c);
    groups(in, c);
    list(in, c);
    check_panel(in, c);
    c.unclip();
    // A click away from the field being edited: a value, the search or the
    // folder applies; a path to save to or open is dropped.
    if (in.pressed && edit_ != kEditNone && edit_ == edit_before && edit_key_ == edit_key_before &&
        !edit_r_.contains(in.mx, in.my)) {
        if (edit_ == kEditSave || edit_ == kEditLoad)
            edit_ = kEditNone;
        else
            commit_edit();
    }
    // A slider let go while its row was off the list (scrolled away): its value applies.
    if (drag_ != kNoKey && !in.down) {
        TextBuf why;
        set_text(drag_, Str(drag_text_), &why);
        drag_ = kNoKey;
    }
    if (popup_)
        popup(in, c, nullptr);
    tooltip(c);
}

void SetupPage::bar(const UiInput& in, Canvas& c) {
    c.fill(bar_, kPanel);
    const i32 h = c.px(26), y = c.px(6), y2 = y + h + c.px(6), gap = c.px(6);
    i32 x = c.px(8);
    char buf[512];
    const i32 ty = y + (h - c.line_h(kFontUi)) / 2, ty2 = y2 + (h - c.line_h(kFontUi)) / 2;
    start_window_ = start_headless_ = Rect{};

    if (read_only()) {
        snprintf(buf, sizeof buf, "Settings of %s", title_[0] ? title_ : source_);
        c.text(x, ty, buf, kFontBold, kText, kAlignLeft, c.width() / 2);
        x += imin(c.text_w(buf, kFontBold), c.width() / 2) + c.px(10);
        c.text(x, ty, "read-only: a run's settings are fixed from its start", kFontSmall, kDim);
        // Right: Back or Copy, and Save as.
        i32 rx = c.width() - c.px(8);
        const Rect save{rx - c.px(84), y, c.px(84), h};
        rx = save.x - gap;
        if (edit_ == kEditSave) {
            const Rect f{c.px(8), y2, imin(c.px(420), c.width() / 3), h};
            field(in, c, f, kEditSave, kNoKey, Str(save_path_), true, kText);
            const Rect ok{f.right() + gap, y2, c.px(64), h};
            if (button(in, c, ok, "Save"))
                commit_edit();
        } else if (button(in, c, save, "Save as")) {
            suggest_save_path();
            begin_edit(kEditSave, kNoKey, Str(save_path_));
        }
        if (mode_ == kSetupArchive) {
            const Rect copy{rx - c.px(150), y, c.px(150), h};
            if (button(in, c, copy, "Copy to a new run")) {
                // The run's settings become a new run's, editable from here.
                for (usize k = 0; k < cur_.size(); ++k)
                    file_[k] = cur_[k];
                char src[260];
                snprintf(src, sizeof src, "%s", title_[0] ? title_ : source_);
                snprintf(source_, sizeof source_, "%s (copy)", src);
                mode_ = kSetupEdit;
                dirty_ = true;
                make_folder_name();
                set_note("a copy of the run's settings: change what you like, then Start");
            }
        } else {
            const Rect back{rx - c.px(130), y, c.px(130), h};
            if (button(in, c, back, "Back to the run")) {
                SetupRequest r = {};
                r.kind = kReqBack;
                reqs_.push(r);
            }
        }
        if (edit_ != kEditSave && info_[0])
            c.text(c.px(8), ty2, info_, kFontUi, kDim, kAlignLeft, c.width() * 2 / 3);
        if (note_[0])
            c.text(c.width() - c.px(10), ty2, note_, kFontUi, note_bad_ ? kBad : kWarn, kAlignRight, c.width() / 3);
        return;
    }

    // Row 1: files.
    const Rect load{x, y, c.px(70), h};
    if (button(in, c, load, "Load  v", popup_ == kPopLoad))
        open_popup(kPopLoad, kNoKey, load);
    x = load.right() + gap;
    const Rect save{x, y, c.px(80), h};
    if (button(in, c, save, "Save as", edit_ == kEditSave) && edit_ != kEditSave) {
        suggest_save_path();
        begin_edit(kEditSave, kNoKey, Str(save_path_));
    }
    x = save.right() + c.px(12);
    if (edit_ == kEditSave || edit_ == kEditLoad) {
        const bool saving = edit_ == kEditSave;
        c.text(x, ty, saving ? "save to" : "open", kFontUi, kDim);
        x += c.px(52);
        const Rect f{x, y, imin(c.px(420), c.width() / 3), h};
        field(in, c, f, edit_, kNoKey, Str(saving ? save_path_ : load_path_), true, kText);
        const Rect ok{f.right() + gap, y, c.px(64), h};
        if (button(in, c, ok, saving ? "Save" : "Open"))
            commit_edit();
        const Rect cancel{ok.right() + gap, y, c.px(64), h};
        if (button(in, c, cancel, "Cancel"))
            edit_ = kEditNone;
    } else {
        const usize ch = changed_count();
        snprintf(buf, sizeof buf, "from %s", source_);
        c.text(x, ty, buf, kFontUi, kText, kAlignLeft, c.width() / 3);
        x += imin(c.text_w(buf, kFontUi), c.width() / 3) + c.px(12);
        if (ch) {
            snprintf(buf, sizeof buf, "%u setting%s changed from it", static_cast<unsigned>(ch), ch == 1 ? "" : "s");
            c.text(x, ty, buf, kFontUi, kAccent);
        }
    }
    if (note_[0])
        c.text(c.width() - c.px(10), ty, note_, kFontUi, note_bad_ ? kBad : kGood, kAlignRight, c.width() / 3);

    // Row 2: the run's folder and Start.
    x = c.px(8);
    c.text(x, ty2, "Run folder", kFontUi, kDim);
    x += c.text_w("Run folder", kFontUi) + c.px(8);
    const Rect ff{x, y2, c.px(280), h};
    field(in, c, ff, kEditFolder, kNoKey, Str(folder_), true, kText, "runs\\<name>");
    x = ff.right() + c.px(2);
    const Rect pick{x, y2, c.px(24), h};
    if (button(in, c, pick, "v", popup_ == kPopFolder, line_count(folders_) > 0))
        open_popup(kPopFolder, kNoKey, Rect{ff.x, ff.y, ff.w, ff.h});
    x = pick.right() + c.px(10);
    const Rect oh{x, y2, c.px(150), h};
    if (checkbox(in, c, oh, outputs_here_, true, "outputs into it"))
        outputs_here_ = !outputs_here_;
    if (oh.contains(in.mx, in.my))
        set_tip("Outputs into the run's folder: each output path that is on (output.*)\n"
                "keeps its file name but goes into the folder, beside run.cfg,\n"
                "so nova view --run <folder> browses it afterwards.");
    x = oh.right() + c.px(8);
    const char* why = start_blocked();
    const bool ok = why[0] == '\0';
    start_window_ = Rect{x, y2, c.px(132), h};
    if (button(in, c, start_window_, "Start in window", false, ok)) {
        SetupRequest r = {};
        r.kind = kReqStartWindow;
        snprintf(r.folder, sizeof r.folder, "%s", folder_);
        reqs_.push(r);
    }
    if (start_window_.contains(in.mx, in.my))
        set_tip("Writes every setting to <folder>\\run.cfg and starts the run here, watched.");
    x = start_window_.right() + gap;
    start_headless_ = Rect{x, y2, c.px(132), h};
    if (button(in, c, start_headless_, "Start headless", false, ok)) {
        SetupRequest r = {};
        r.kind = kReqStartHeadless;
        snprintf(r.folder, sizeof r.folder, "%s", folder_);
        reqs_.push(r);
    }
    if (start_headless_.contains(in.mx, in.my))
        set_tip("Writes every setting to <folder>\\run.cfg and starts nova run <folder>\\run.cfg\n"
                "in a console of its own, for long runs; creating <folder>\\stop (or Esc in\n"
                "its console) stops it, with every output finished.");
    x = start_headless_.right() + c.px(10);
    if (ok)
        c.text(x, ty2, "ready", kFontUi, kGood);
    else
        c.text(x, ty2, why, kFontUi, kBad);
    x += c.text_w(ok ? "ready" : why, kFontUi) + c.px(16);
    if (headless_[0]) {
        snprintf(buf, sizeof buf, "headless run: %s", headless_);
        const i32 tw = imin(c.text_w(buf, kFontSmall), c.px(300));
        c.text(x, y2 + (h - c.line_h(kFontSmall)) / 2, buf, kFontSmall, kDim, kAlignLeft, tw);
        const Rect stop{x + tw + c.px(6), y2, c.px(70), h};
        if (stop.right() < c.width() && button(in, c, stop, "Stop it")) {
            SetupRequest r = {};
            r.kind = kReqStopHeadless;
            snprintf(r.folder, sizeof r.folder, "%s", headless_);
            reqs_.push(r);
        }
    }
}

void SetupPage::groups(const UiInput& in, Canvas& c) {
    Rect inner;
    panel_box(c, groups_r_, "Groups", &inner);
    const i32 rh = c.line_h(kFontUi) + c.px(8);
    // Per group: changed settings and problems.
    u32 n_changed[64] = {}, n_problems[64] = {};
    const SettingsCheck& ch = check();
    for (usize k = 0; k < cur_.size(); ++k) {
        const u32 g = group_of(key_at(k));
        if (g < 64 && changed(k))
            ++n_changed[g];
    }
    for (usize i = 0; i < ch.problems.size(); ++i) {
        const Key* k = key_find(ch.problems[i].key);
        const u32 g = k ? group_of(*k) : 0;
        if (k && g < 64)
            ++n_problems[g];
    }
    const bool listing = search_[0] || changed_only_;
    i32 y = inner.y;
    c.clip(inner);
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!group_shown(g))
            continue;
        const Rect r{inner.x, y, inner.w, rh};
        const bool sel = g == group_ && !listing;
        if (sel)
            c.fill(r, kRowSel);
        else if (r.contains(in.mx, in.my))
            c.fill(r, kRowHot);
        if (r.contains(in.mx, in.my)) {
            cursor_ = 3;
            if (in.pressed) {
                select_group(g);
                if (edit_ == kEditSearch)
                    edit_ = kEditNone;
            }
        }
        c.text(r.x + c.px(6), r.y + c.px(4), kKeyGroups[g], kFontUi, sel ? 0xFFFFFF : kText, kAlignLeft,
               r.w - c.px(50));
        char b[32];
        i32 rx = r.right() - c.px(6);
        if (g < 64 && n_problems[g]) {
            snprintf(b, sizeof b, "%u!", n_problems[g]);
            c.text(rx, r.y + c.px(4), b, kFontBold, kBad, kAlignRight);
            rx -= c.text_w(b, kFontBold) + c.px(6);
        }
        if (g < 64 && n_changed[g]) {
            snprintf(b, sizeof b, "%u", n_changed[g]);
            c.text(rx, r.y + c.px(5), b, kFontSmall, kAccent, kAlignRight);
        }
        y += rh;
    }
    c.unclip();
    if (inner.bottom() - y > c.line_h(kFontSmall) * 3) {
        c.text(inner.x + c.px(4), inner.bottom() - c.line_h(kFontSmall) * 2, "blue: changed from the file", kFontSmall,
               kAccent);
        c.text(inner.x + c.px(4), inner.bottom() - c.line_h(kFontSmall), "red: problems", kFontSmall, kBad);
    }
}

void SetupPage::build_rows(Canvas& c) {
    rows_.clear();
    const bool listing = search_[0] || changed_only_;
    const SettingsCheck& ch = check();
    const i32 rh = c.px(30), ph = c.line_h(kFontSmall) + c.px(2), hh = c.line_h(kFontBold) + c.px(10);
    i32 y = 0;
    u32 last_group = 0xFFFFFFFFu;
    for (u32 g = 0; g < kKeyGroupCount; ++g) {
        if (!listing && g != group_)
            continue;
        for (usize k = 0; k < cur_.size(); ++k) {
            const Key& key = key_at(k);
            if ((key.flags & kKeyHidden) || group_of(key) != g)
                continue;
            if (search_[0] && !matches(k))
                continue;
            if (changed_only_ && !changed(k))
                continue;
            if (listing && g != last_group) {
                Row hdr{kNoKey, g, y, hh};
                rows_.push(hdr);
                y += hh;
                last_group = g;
            }
            i32 h = rh;
            for (usize i = 0; i < ch.problems.size(); ++i)
                if (Str(key.name).eq(ch.problems[i].key))
                    h += ph;
            Row r{k, g, y, h};
            rows_.push(r);
            y += h;
        }
    }
    content_h_ = y;
    if (jump_ != kNoKey) {
        for (usize i = 0; i < rows_.size(); ++i)
            if (rows_[i].key == jump_)
                scroll_ = rows_[i].y - c.px(30);
        jump_ = kNoKey;
    }
}

void SetupPage::list(const UiInput& in, Canvas& c) {
    char title[200];
    if (search_[0] && changed_only_)
        snprintf(title, sizeof title, "Changed settings matching \"%s\"", search_);
    else if (search_[0])
        snprintf(title, sizeof title, "Settings matching \"%s\"", search_);
    else if (changed_only_)
        snprintf(title, sizeof title, "Settings changed from the file");
    else
        snprintf(title, sizeof title, "%s", kKeyGroups[group_]);
    Rect inner;
    panel_box(c, list_r_, title, &inner);
    // The search box and the changed-only filter.
    const i32 fh = c.px(24);
    const Rect sf{inner.x, inner.y, imin(c.px(280), inner.w / 2), fh};
    field(in, c, sf, kEditSearch, kNoKey, Str(search_), true, kText, "search names, labels and help");
    const Rect co{sf.right() + c.px(8), inner.y, c.px(120), fh};
    if (button(in, c, co, "changed only", changed_only_)) {
        changed_only_ = !changed_only_;
        scroll_ = 0;
    }
    build_rows(c);
    u32 shown_n = 0;
    for (usize i = 0; i < rows_.size(); ++i)
        shown_n += rows_[i].key != kNoKey;
    char b[64];
    snprintf(b, sizeof b, "%u setting%s", shown_n, shown_n == 1 ? "" : "s");
    c.text(co.right() + c.px(10), inner.y + (fh - c.line_h(kFontSmall)) / 2, b, kFontSmall, kDim);

    const Rect area{inner.x, inner.y + fh + c.px(6), inner.w, inner.bottom() - (inner.y + fh + c.px(6))};
    Rect track;
    scroll_split(c, area, &rows_r_, &track);
    // Scrolling: the wheel, or the scrollbar (the viewer's too: widgets.h), in pixels.
    const bool bar_held = list_bar_.update(in, c, area, track, content_h_, rows_r_.h, c.px(30) * 3, &scroll_);
    if (rows_.empty()) {
        c.text(rows_r_.x + c.px(4), rows_r_.y + c.px(4),
               search_[0] ? "no setting matches" : (changed_only_ ? "nothing changed from the file" : "no settings here yet"),
               kFontUi, kDim);
        return;
    }
    UiInput rin = in;
    if (bar_held) {
        rin.pressed = false;
        rin.wheel = 0;
    }
    c.clip(rows_r_);
    for (usize i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const Rect rr{rows_r_.x, rows_r_.y + r.y - scroll_, rows_r_.w, r.h};
        if (rr.bottom() < rows_r_.y || rr.y > rows_r_.bottom())
            continue;
        if (r.key == kNoKey) {
            c.fill(Rect{rr.x, rr.y + rr.h - c.px(2), rr.w, 1}, kFaint);
            c.text(rr.x + c.px(4), rr.y + c.px(4), kKeyGroups[r.group], kFontBold, kAccent);
            continue;
        }
        // Rows partly outside take no clicks (their controls may be cut); the
        // mouse outside the list is nowhere, except for a slider being dragged.
        UiInput ri = rin;
        if (rr.y < rows_r_.y || rr.bottom() > rows_r_.bottom() + 1)
            ri.pressed = false;
        if (!rows_r_.contains(in.mx, in.my) && drag_ != r.key) {
            ri.mx = -1;
            ri.my = -1;
            ri.pressed = false;
        }
        draw_row(ri, c, r, rr);
    }
    c.unclip();
}

void SetupPage::draw_row(const UiInput& in, Canvas& c, const Row& r, const Rect& rr) {
    const usize k = r.key;
    const Key& key = key_at(k);
    const bool grey = greyed(k);
    const bool ro = read_only();
    const bool enabled = !grey && !ro;
    ro_row_ = ro && !grey;
    const bool chg = changed(k);
    const SettingsCheck& ch = check();
    const SettingProblem* prob = nullptr;
    for (usize i = 0; i < ch.problems.size() && !prob; ++i)
        if (Str(key.name).eq(ch.problems[i].key))
            prob = &ch.problems[i];
    row_r_[k] = rr;
    const i32 base = c.px(30);
    const bool hot = rr.contains(in.mx, in.my);
    if (hot)
        c.fill(rr, kRowHot);
    if (prob)
        c.fill(Rect{rr.x, rr.y + c.px(2), c.px(3), rr.h - c.px(4)}, kBad);
    else if (chg)
        c.fill(Rect{rr.x, rr.y + c.px(2), c.px(3), base - c.px(4)}, kAccent);

    // Columns, scaled to the list's width.
    const i32 avail = rr.w - c.px(12);
    const i32 lw = imax(c.px(130), imin(c.px(230), avail * 27 / 100));
    i32 cw = imax(c.px(200), imin(c.px(360), avail * 42 / 100));
    i32 dw = c.px(150);
    // A narrow list: the default's column gives way first, then the control,
    // so the reset button always fits.
    const i32 fixed = c.px(10 + 6 + 12 + 2 + 14 + 50 + 6);
    if (lw + cw + dw + fixed > rr.w)
        dw = imax(c.px(90), rr.w - lw - cw - fixed);
    if (lw + cw + dw + fixed > rr.w)
        cw = imax(c.px(170), rr.w - lw - dw - fixed);
    const i32 x0 = rr.x + c.px(10);
    const i32 cx = x0 + lw + c.px(6);
    const i32 dx = cx + cw + c.px(12);
    const i32 ctl_h = c.px(22);
    const i32 cy = rr.y + (base - ctl_h) / 2;
    const u32 label_col = grey ? kFaint : kText;
    i32 label_w = lw - c.px(4);
    if ((key.flags & kKeyLog) && (key.kind == KeyKind::number || key.kind == KeyKind::whole)) {
        // A logarithmic knob says so, at the end of its label's column.
        c.text(x0 + lw - c.px(6), rr.y + (base - c.line_h(kFontSmall)) / 2, "log", kFontSmall, kFaint, kAlignRight);
        label_w -= c.text_w("log", kFontSmall) + c.px(6);
    }
    c.text(x0, rr.y + (base - c.line_h(kFontUi)) / 2, key.label, kFontUi, label_col, kAlignLeft, label_w);

    // The control.
    const bool computed_unset = (key.flags & kKeyComputed) && !cur_[k].set;
    Str v = shown(k);
    char compact[48];
    if (computed_unset && key.kind == KeyKind::number) { // shown to 6 figures (run.cfg has it exact)
        snprintf(compact, sizeof compact, "%.6g", num_of(v));
        v = Str(compact);
    }
    const u32 vcol = computed_unset ? kDim : kText;
    Rect ctl{cx, cy, cw, ctl_h};
    switch (key.kind) {
    case KeyKind::flag: {
        bool on = false;
        parse_bool(v, &on);
        ctl = Rect{cx, cy, c.px(90), ctl_h};
        if (checkbox(in, c, ctl, on, enabled, on ? "on" : "off")) {
            TextBuf why;
            set_text(k, on ? "false" : "true", &why);
        }
        break;
    }
    case KeyKind::whole:
    case KeyKind::number: {
        const i32 fw = c.px(110);
        const Rect track{cx + c.px(6), cy, cw - fw - c.px(18), ctl_h};
        slider(in, c, k, track, enabled);
        const Rect nf{cx + cw - fw, cy, fw, ctl_h};
        const Str txt = drag_ == k ? Str(drag_text_) : v;
        field(in, c, nf, kEditValue, k, txt, enabled, vcol);
        break;
    }
    case KeyKind::choice: {
        ctl = Rect{cx, cy, imin(cw, c.px(240)), ctl_h};
        char b[128];
        snprintf(b, sizeof b, "%.*s", static_cast<int>(v.n < 100 ? v.n : 100), v.p);
        const bool open = popup_ == kPopChoice && popup_key_ == k;
        const bool h2 = enabled && ctl.contains(in.mx, in.my);
        c.fill(ctl, open ? kButtonOn : (h2 ? kButtonHot : (enabled ? kButton : kPanel)));
        c.text(ctl.x + c.px(8), ctl.y + (ctl.h - c.line_h(kFontUi)) / 2, b, kFontUi, enabled ? vcol : off_col(),
               kAlignLeft, ctl.w - c.px(28));
        c.text(ctl.right() - c.px(8), ctl.y + (ctl.h - c.line_h(kFontUi)) / 2, "v", kFontUi, kFaint, kAlignRight);
        if (h2) {
            cursor_ = 3;
            if (in.pressed)
                open_popup(kPopChoice, k, ctl);
        }
        break;
    }
    case KeyKind::text:
        if (Str(key.name).eq("world.seed") && !ro) { // the map's seed: and a new one in a click
            const i32 bw = c.px(84);
            const Rect fr{cx, cy, cw - bw - c.px(6), ctl_h};
            field(in, c, fr, kEditValue, k, v, enabled, vcol, "(the seed's map)");
            if (button(in, c, Rect{cx + cw - bw, cy, bw, ctl_h}, "new map", false, enabled))
                new_map();
            break;
        }
        field(in, c, ctl, kEditValue, k, v, enabled, vcol, "(empty)");
        break;
    }
    ctl_r_[k] = Rect{cx, cy, cw, ctl_h};

    // The default: a click takes it.
    char d[160];
    if (key.flags & kKeyComputed)
        snprintf(d, sizeof d, computed_unset ? "computed: %.*s" : "computed", static_cast<int>(v.n < 40 ? v.n : 40), v.p);
    else
        snprintf(d, sizeof d, "default %s", key.def_text[0] ? key.def_text : "(empty)");
    const Rect dr{dx, rr.y, dw, base};
    const bool dhot = enabled && dr.contains(in.mx, in.my) && !computed_unset;
    c.text(dx, rr.y + (base - c.line_h(kFontSmall)) / 2, d, kFontSmall, dhot ? kText : kDim, kAlignLeft, dw - c.px(4));
    if (dhot) {
        cursor_ = 3;
        if (in.pressed)
            unset(k);
    }
    // Changed from the file: a mark and a reset.
    const i32 mxp = dx + dw + c.px(2);
    if (chg)
        c.fill(Rect{mxp, rr.y + base / 2 - c.px(4), c.px(8), c.px(8)}, kAccent);
    const Rect rb{mxp + c.px(14), rr.y + (base - ctl_h) / 2, c.px(50), ctl_h};
    if (rb.right() <= rr.right() && (chg && !ro)) {
        if (button(in, c, rb, "reset"))
            reset(k);
    }
    if (prob) {
        char p[300];
        snprintf(p, sizeof p, "%s", prob->what);
        c.text(cx, rr.y + base - c.px(2), p, kFontSmall, kBad, kAlignLeft, rr.right() - cx - c.px(4));
    }

    // Hovering: the key's name and its help.
    if (hot && !tip_[0] && drag_ == kNoKey) {
        TextBuf t;
        t.append(key.name);
        t.append("\n");
        // the help, wrapped at about 72 characters
        const Str h(key.help);
        usize at = 0;
        while (at < h.n) {
            usize end = at + 72 < h.n ? at + 72 : h.n;
            if (end < h.n) {
                usize sp = end;
                while (sp > at && h.p[sp] != ' ')
                    --sp;
                end = sp > at ? sp : end;
            }
            t.append(h.sub(at, end - at));
            t.append("\n");
            at = end;
            while (at < h.n && h.p[at] == ' ')
                ++at;
        }
        t.append(key_kind_name(key.kind));
        if (key.flags & kKeyLog)
            t.append(", log scale");
        t.append(": ");
        append_range(t, key);
        if (key.needs[0]) {
            t.append("\nneeds ");
            t.append(key.needs);
            if (grey)
                t.append(" (off: greyed out, the value kept)");
        }
        if (computed_unset) {
            t.append("\nthe run works it out: ");
            t.append(v);
        }
        set_tip(t.c_str());
    }
    ro_row_ = false;
}

void SetupPage::check_panel(const UiInput& in, Canvas& c) {
    Rect inner;
    panel_box(c, check_r_, "Check", &inner);
    const SettingsCheck& ch = check();
    const i32 lh = c.line_h(kFontSmall) + c.px(1);
    { // The wheel or the scrollbar, in pixels (check_h_: the last frame's height).
        Rect track;
        const Rect area = inner;
        scroll_split(c, area, &inner, &track);
        check_bar_.update(in, c, area, track, check_h_, inner.h, lh * 3, &check_scroll_);
    }
    c.clip(inner);
    i32 y = inner.y - check_scroll_;
    char b[512];
    auto text = [&](const char* s, u32 font, u32 col) {
        const i32 h = c.line_h(font) + c.px(1);
        wrap_text(c, s, inner.w - c.px(4), font, [&](usize at, usize n) {
            c.text(inner.x + c.px(2), y, s + at, n, font, col);
            y += h;
        });
    };
    auto head = [&](const char* s) {
        y += c.px(6);
        c.text(inner.x, y, s, kFontBold, kText);
        y += c.line_h(kFontBold) + c.px(2);
    };
    const usize np = ch.problems.size();
    if (read_only()) {
        text("A run's settings are fixed from its start: this is what it ran with.", kFontSmall, kDim);
    } else if (np == 0) {
        text("No problems: ready to start.", kFontUi, kGood);
    } else {
        snprintf(b, sizeof b, "%u problem%s: Start stays off until %s fixed.", static_cast<unsigned>(np),
                 np == 1 ? "" : "s", np == 1 ? "it is" : "they are");
        text(b, kFontUi, kBad);
    }
    for (usize i = 0; i < np; ++i) {
        const SettingProblem& p = ch.problems[i];
        const usize k = key_index(p.key);
        y += c.px(3);
        if (p.key[0]) {
            const Rect kr{inner.x, y, inner.w, c.line_h(kFontBold)};
            const bool hot = k != kNoKey && kr.contains(in.mx, in.my);
            c.text(inner.x + c.px(2), y, p.key, kFontBold, hot ? 0xFFFFFF : kBad);
            if (hot) {
                cursor_ = 3;
                if (in.pressed)
                    jump_to(k);
            }
            y += c.line_h(kFontBold);
        }
        text(p.what, kFontSmall, kText);
        if (!p.key[0] && unknown_.size() && !read_only() && strstr(p.what, "is not a setting")) {
            const Rect drop{inner.x + c.px(2), y + c.px(2), c.px(150), c.px(22)};
            if (button(in, c, drop, "drop unknown keys")) {
                unknown_.clear();
                dirty_ = redraw_ = true;
            }
            y += c.px(26);
        }
    }

    // The world's size.
    head("The world");
    if (ch.soup) {
        const Str nodes = shown(key_index("world.nodes")), graph = shown(key_index("world.graph"));
        snprintf(b, sizeof b, "%.*s nodes, a %.*s graph, about %llu codons of memory a node", static_cast<int>(nodes.n),
                 nodes.p, static_cast<int>(graph.n), graph.p, static_cast<unsigned long long>(ch.soup_size));
        text(b, kFontSmall, kText);
        char need[32], have[32];
        bytes_text(ch.ram, need, sizeof need);
        bytes_text(ch.ram_avail, have, sizeof have);
        const bool fits = ch.ram <= 0.9 * ch.ram_avail;
        snprintf(b, sizeof b, "memory: about %s%s, of %s available now (a world may take 90%%)", need,
                 ch.disk_in_file ? "" : " (the disk tier in RAM included)", have);
        text(b, kFontSmall, fits ? kText : kBad);
        // (The disk tier: the fast disk and the archive, docs/world-plan.md W4.)
        if (ch.disk + ch.archive == 0) {
            text("disk tier: none (world.disk = 0, world.archive = 0)", kFontSmall, kDim);
        } else if (ch.disk_in_file) {
            char f[32], fr[32];
            bytes_text(ch.disk_file, f, sizeof f);
            bytes_text(ch.disk_free, fr, sizeof fr);
            const Str dp = shown(key_index("disk.path"));
            snprintf(b, sizeof b, "disk tier: about %s in files under %.*s (%s free there)", f,
                     static_cast<int>(dp.n), dp.p, fr);
            text(b, kFontSmall, ch.disk_file <= 0.95 * ch.disk_free ? kText : kBad);
        } else {
            snprintf(b, sizeof b, "disk tier: %llu codons a node (%llu fast disk, %llu archive), in RAM (disk.path empty)",
                     static_cast<unsigned long long>(ch.disk + ch.archive), static_cast<unsigned long long>(ch.disk),
                     static_cast<unsigned long long>(ch.archive));
            text(b, kFontSmall, kDim);
        }
    } else {
        text("the demo world (world.model = demo): parcels, no soup", kFontSmall, kDim);
    }
    if (ch.notes.size()) {
        TextBuf nb;
        nb.append(ch.notes.view());
        text(nb.c_str(), kFontSmall, kDim);
    }

    // The landscape.
    map_r_ = new_map_r_ = Rect{};
    if (ch.landscape) {
        head("The landscape");
        const LandSummary& L = ch.land;
        if (!read_only()) { // a new map, beside the heading
            const i32 bw = c.px(84), bh = c.px(20);
            new_map_r_ = Rect{inner.right() - bw - c.px(2), y - c.line_h(kFontBold) - c.px(4), bw, bh};
            if (button(in, c, new_map_r_, "new map"))
                new_map();
        }
        {
            const Str ms = shown(key_index("world.seed"));
            if (ms.n)
                snprintf(b, sizeof b, "map seed %.*s (world.seed; the run's own draws follow seed)",
                         static_cast<int>(ms.n < 200 ? ms.n : 200), ms.p);
            else
                snprintf(b, sizeof b, "the map follows seed (set world.seed, or new map, for another)");
            text(b, kFontSmall, kDim);
        }
        if (!L.built) {
            text("it can't be generated with these settings (see the problems)", kFontSmall, kBad);
        } else {
            // The map: land and water as the viewer draws them, the paths
            // (bridges in amber) and the nodes.
            const usize cells = static_cast<usize>(L.raster_w) * L.raster_h;
            if (cells && ch.land_cells.size() == cells) {
                if (map_gen_ != ch.land_gen || map_img_.size() != cells) {
                    map_img_.resize(cells);
                    for (usize i = 0; i < cells; ++i) {
                        const u32 m = ch.land_cells[i];
                        map_img_[i] = m == 0xFFFFFFFFu ? 0x0E1A2C : mix_color(0x37442F, lineage_color(m + 1), 0.12);
                    }
                    map_gen_ = ch.land_gen;
                }
                const i32 mw = inner.w - c.px(4);
                const i32 mh = static_cast<i32>(static_cast<double>(mw) * L.raster_h / L.raster_w + 0.5);
                map_r_ = Rect{inner.x + c.px(2), y + c.px(4), mw, mh};
                c.image(map_r_, map_img_.data(), static_cast<i32>(L.raster_w), static_cast<i32>(L.raster_h));
                const double sx = mw / (L.width > 0 ? L.width : 1.0), sy = mh;
                auto px = [&](u32 n) { return map_r_.x + static_cast<i32>(ch.land_x[n] * sx); };
                auto py = [&](u32 n) { return map_r_.y + static_cast<i32>(ch.land_y[n] * sy); };
                const u32 nn = static_cast<u32>(ch.land_x.size());
                for (usize e = 0; e + 1 < ch.land_paths.size(); e += 2) {
                    const u32 a = ch.land_paths[e], bnode = ch.land_paths[e + 1];
                    if (a >= nn || bnode >= nn)
                        continue;
                    if (ch.land_bridge[e / 2])
                        c.dashed(px(a), py(a), px(bnode), py(bnode), 0xE8A840, c.px(4));
                    else
                        c.line(px(a), py(a), px(bnode), py(bnode), 0x76836F);
                }
                const i32 d = c.px(2) > 2 ? c.px(2) : 2;
                for (u32 n = 0; n < nn; ++n)
                    c.fill(Rect{px(n) - d / 2, py(n) - d / 2, d, d}, 0xE6EAF0);
                y += mh + c.px(8);
            }
            snprintf(b, sizeof b, "%.2f x 1, a %ux%u raster, %.0f%% land (generated in %.0f ms)", L.width, L.raster_w,
                     L.raster_h, 100.0 * L.land_share, L.ms);
            text(b, kFontSmall, kText);
            i32 n = snprintf(b, sizeof b, "%u nodes on %u landmasses (of %u); the largest:", L.nodes, L.peopled, L.masses);
            for (u32 k = 0; k < 8 && L.largest[k]; ++k)
                n += snprintf(b + n, sizeof b - n, " %u", L.largest[k]);
            text(b, kFontSmall, kText);
            n = snprintf(b, sizeof b, "land paths a node: %.2f on average;", L.paths_mean);
            for (u32 d = 0; d < 14; ++d)
                if (L.hist[d])
                    n += snprintf(b + n, sizeof b - n, "  %u%s: %u", d, d == 13 ? "+" : "", L.hist[d]);
            text(b, kFontSmall, kText);
            snprintf(b, sizeof b, "%u hermits, %u hubs, %u long links; %u bridges (open %u of every %u ticks)",
                     L.hermits, L.hubs, L.long_links, L.bridges, L.open_ticks, L.open_every);
            text(b, kFontSmall, kText);
            // Substrates (docs/world-plan.md W2, W8), as nova land counts them.
            if (L.substrates)
                snprintf(b, sizeof b, "substrates: %u distinct over %u nodes (of 390625 possible)",
                         L.substrates_distinct, L.nodes);
            else
                snprintf(b, sizeof b, "substrates: off (substrate.enabled = false)");
            text(b, kFontSmall, L.substrates ? kText : kDim);
        }
    }

    // Computed defaults: what the run will work out.
    head("Worked out by the run");
    u32 shown_n = 0;
    for (usize k = 0; k < cur_.size(); ++k) {
        const Key& key = key_at(k);
        if (!(key.flags & kKeyComputed) || (key.flags & kKeyHidden) || cur_[k].set || !ch.read[k])
            continue;
        const Str e = ch.effective(k);
        snprintf(b, sizeof b, "%s = %.*s", key.name, static_cast<int>(e.n), e.p);
        text(b, kFontSmall, kDim);
        ++shown_n;
    }
    if (!shown_n)
        text("(every computed setting is set by hand, or its feature is off)", kFontSmall, kFaint);
    check_h_ = y + check_scroll_ - inner.y + c.px(8);
    c.unclip();
}

// --- the popup and the tooltip -----------------------------------------------------------

void SetupPage::popup(const UiInput& in, Canvas& c, bool* consumed) {
    if (!popup_)
        return;
    u32 count = 0;
    if (popup_ == kPopChoice && popup_key_ < cur_.size()) {
        while (option_at(key_at(popup_key_).options, count).n)
            ++count;
    } else if (popup_ == kPopLoad) {
        count = line_count(presets_) + 1; // + open a file by its path
    } else if (popup_ == kPopFolder) {
        count = line_count(folders_);
    }
    const i32 ih = c.line_h(kFontUi) + c.px(6);
    const i32 w = imax(popup_r_.w, c.px(popup_ == kPopChoice ? 200.0f : 340.0f));
    const i32 max_rows = imax(4, (c.height() - popup_r_.y - c.px(8)) / ih);
    const i32 rows = static_cast<i32>(count) < max_rows ? static_cast<i32>(count) : max_rows;
    Rect r{popup_r_.x, popup_r_.y, w, rows * ih + c.px(4)};
    if (r.right() > c.width())
        r.x = c.width() - r.w;
    if (r.bottom() > c.height())
        r.y = c.height() - r.h;
    popup_r_ = Rect{r.x, r.y, popup_r_.w, 0};
    // A scrollbar down its right side when not every item fits (2026-11-09).
    const i32 bar_w = static_cast<i32>(count) > rows ? c.px(kScrollBarW) + c.px(kScrollBarGap) : 0;
    const Rect track{r.right() - c.px(2) - c.px(kScrollBarW), r.y + c.px(2), c.px(kScrollBarW), r.h - c.px(4)};
    bool held = false; // the scrollbar has the mouse: no item takes the click
    if (consumed) {
        held = popup_bar_.input(in, c, r, track, static_cast<i32>(count), rows, 3, &popup_scroll_);
        if (held || (in.wheel && r.contains(in.mx, in.my)))
            *consumed = true;
    }
    popup_scroll_ = scroll_clamp(static_cast<i32>(count), rows, popup_scroll_);
    const u32 kind = popup_;
    auto label = [&](u32 item, char* out, usize n) {
        if (kind == kPopChoice) {
            const Str o = option_at(key_at(popup_key_).options, item);
            snprintf(out, n, "%.*s", static_cast<int>(o.n), o.p);
        } else if (kind == kPopLoad) {
            if (item + 1 == count)
                snprintf(out, n, "open a file by its path...");
            else {
                const Str l = line_at(presets_, item);
                snprintf(out, n, "%.*s", static_cast<int>(l.n), l.p);
            }
        } else {
            const Str l = line_at(folders_, item);
            snprintf(out, n, "%.*s", static_cast<int>(l.n), l.p);
        }
    };
    if (consumed && in.pressed && !held) {
        *consumed = true;
        popup_ = kPopNone;
        if (!r.contains(in.mx, in.my))
            return;
        const u32 item = static_cast<u32>((in.my - r.y - c.px(2)) / ih + popup_scroll_);
        if (item >= count)
            return;
        char l[300];
        label(item, l, sizeof l);
        if (kind == kPopChoice) {
            TextBuf why;
            if (!read_only() && !greyed(popup_key_))
                set_text(popup_key_, Str(l), &why);
        } else if (kind == kPopLoad) {
            if (item + 1 == count) {
                if (!load_path_[0])
                    snprintf(load_path_, sizeof load_path_, "configs\\");
                begin_edit(kEditLoad, kNoKey, Str(load_path_));
            } else {
                TextBuf err;
                if (load_file(l, &err))
                    set_note("");
                else
                    set_note(err.c_str(), true);
            }
        } else if (kind == kPopFolder) {
            set_folder(l);
        }
        return;
    }
    if (consumed)
        return; // the input pass: drawn last, over everything
    c.fill(r, kPopup);
    c.frame(r, kFaint);
    popup_bar_.draw(in, c, track, static_cast<i32>(count), rows, popup_scroll_);
    char cur[300] = "";
    if (popup_ == kPopChoice) {
        const Str v = shown(popup_key_);
        snprintf(cur, sizeof cur, "%.*s", static_cast<int>(v.n), v.p);
    }
    for (i32 i = 0; i < rows; ++i) {
        const u32 item = static_cast<u32>(i + popup_scroll_);
        if (item >= count)
            break;
        const Rect ir{r.x + c.px(2), r.y + c.px(2) + i * ih, r.w - c.px(4) - bar_w, ih};
        char l[300];
        label(item, l, sizeof l);
        const bool on = popup_ == kPopChoice && strcmp(l, cur) == 0;
        if (ir.contains(in.mx, in.my))
            c.fill(ir, kButtonHot);
        if (on)
            c.fill(Rect{ir.x, ir.y + c.px(3), c.px(3), ih - c.px(6)}, kAccent);
        c.text(ir.x + c.px(8), ir.y + c.px(3), l, kFontUi, on ? 0xFFFFFF : kText, kAlignLeft, ir.w - c.px(10));
    }
}

void SetupPage::tooltip(Canvas& c) {
    if (!tip_[0])
        return;
    const i32 lh = c.line_h(kFontSmall);
    i32 w = 0, lines = 0;
    const char* s = tip_;
    while (*s) {
        const char* e = s;
        while (*e && *e != '\n')
            ++e;
        const i32 tw = c.text_w(s, static_cast<usize>(e - s), kFontSmall);
        w = tw > w ? tw : w;
        ++lines;
        s = *e ? e + 1 : e;
    }
    Rect r{tip_x_ + c.px(14), tip_y_ + c.px(14), w + c.px(12), lines * lh + c.px(8)};
    if (r.right() > c.width())
        r.x = tip_x_ - r.w - c.px(8);
    if (r.bottom() > c.height())
        r.y = tip_y_ - r.h - c.px(8);
    c.fill(r, kTipBg);
    c.frame(r, kFaint);
    i32 y = r.y + c.px(4);
    s = tip_;
    bool first = true;
    while (*s) {
        const char* e = s;
        while (*e && *e != '\n')
            ++e;
        c.text(r.x + c.px(6), y, s, static_cast<usize>(e - s), first ? kFontBold : kFontSmall, first ? kAccent : kText);
        first = false;
        y += lh;
        s = *e ? e + 1 : e;
    }
}

} // namespace nova
