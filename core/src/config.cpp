#include "nova/config.h"

#include <cstdio> // snprintf

#include "nova/io.h"

namespace nova {

namespace {

// Clamp a view length for "%.*s" so long junk values can't overflow messages.
int show(Str s) {
    return s.n > 64 ? 64 : static_cast<int>(s.n);
}

// The process-wide fallback defaults (Config::set_fallback), or nullptr.
Config* g_fallback = nullptr;

} // namespace

bool Config::set_fallback(const char* text, ParseError* err) {
    if (!text) {
        delete g_fallback;
        g_fallback = nullptr;
        return true;
    }
    Config* f = new Config;
    if (!f->parse(Str(text), err)) {
        delete f;
        return false;
    }
    delete g_fallback;
    g_fallback = f;
    return true;
}

bool Config::valid_key(Str key) {
    if (key.n == 0 || key.n > 128)
        return false;
    for (usize i = 0; i < key.n; ++i) {
        char c = key.p[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void Config::clear() {
    arena_.clear();
    entries_.clear();
    value_error_[0] = '\0';
}

u32 Config::store(Str s) {
    usize off = arena_.size();
    if (off + s.n > 0xFFFFFFFFull)
        NOVA_FATAL("Config: text arena over 4 GB");
    arena_.append(s.p, s.n);
    return static_cast<u32>(off);
}

Str Config::key(usize i) const {
    const Entry& e = entries_[i];
    return Str(arena_.data() + e.key_off, e.key_len);
}

Str Config::value(usize i) const {
    const Entry& e = entries_[i];
    return Str(arena_.data() + e.val_off, e.val_len);
}

i64 Config::index_of(Str k) const {
    for (usize i = 0; i < entries_.size(); ++i)
        if (key(i).eq(k))
            return static_cast<i64>(i);
    return -1;
}

bool Config::parse(Str text, ParseError* err) {
    clear();
    u32 line = 0;
    usize i = 0;
    while (i < text.n) {
        usize end = i;
        while (end < text.n && text.p[end] != '\n')
            ++end;
        ++line;
        Str raw = text.sub(i, end - i);
        i = end + 1;

        raw = raw.sub(0, raw.find('#')).trim();
        if (raw.empty())
            continue;

        usize eq = raw.find('=');
        if (eq == raw.n) {
            err->line = line;
            snprintf(err->msg, sizeof err->msg, "line %u: expected 'key = value', got '%.*s'", line,
                     show(raw), raw.p);
            clear();
            return false;
        }
        Str k = raw.sub(0, eq).trim();
        Str v = raw.sub(eq + 1, raw.n - eq - 1).trim();
        if (!valid_key(k)) {
            err->line = line;
            snprintf(err->msg, sizeof err->msg,
                     "line %u: invalid key '%.*s' (use letters, digits, '_', '.', '-')", line,
                     show(k), k.p);
            clear();
            return false;
        }
        i64 prev = index_of(k);
        if (prev >= 0) {
            err->line = line;
            snprintf(err->msg, sizeof err->msg,
                     "line %u: duplicate key '%.*s' (first set on line %u)", line, show(k), k.p,
                     entries_[static_cast<usize>(prev)].line);
            clear();
            return false;
        }
        Entry e;
        e.key_off = store(k);
        e.key_len = static_cast<u32>(k.n);
        e.val_off = store(v);
        e.val_len = static_cast<u32>(v.n);
        e.line = line;
        e.used = 0;
        entries_.push(e);
    }
    return true;
}

bool Config::load_file(const char* path, ParseError* err) {
    Array<u8> bytes;
    if (!read_file(path, &bytes)) {
        clear();
        err->line = 0;
        snprintf(err->msg, sizeof err->msg, "cannot read config file '%s'", path);
        return false;
    }
    return parse(Str(reinterpret_cast<const char*>(bytes.data()), bytes.size()), err);
}

bool Config::lookup(Str k, Str* v) {
    i64 idx = index_of(k);
    if (idx < 0) {
        if (g_fallback && g_fallback != this) { // Phase 10: the fallback defaults
            const i64 f = g_fallback->index_of(k);
            if (f >= 0) {
                *v = g_fallback->value(static_cast<usize>(f));
                return true;
            }
        }
        return false;
    }
    entries_[static_cast<usize>(idx)].used = 1;
    *v = value(static_cast<usize>(idx));
    return true;
}

void Config::fail(Str k, const char* what) {
    if (value_error_[0] != '\0')
        return; // keep the first problem
    snprintf(value_error_, sizeof value_error_, "config: %.*s: %s", show(k), k.p, what);
}

void Config::bad_value(Str k, Str v, const char* expected) {
    char what[160];
    snprintf(what, sizeof what, "expected %s, got '%.*s'", expected, show(v), v.p);
    fail(k, what);
}

u64 Config::get_u64(Str k, u64 def) {
    Str v;
    u64 out;
    if (!lookup(k, &v))
        return def;
    if (!parse_u64(v, &out)) {
        bad_value(k, v, "an unsigned integer");
        return def;
    }
    return out;
}

i64 Config::get_i64(Str k, i64 def) {
    Str v;
    i64 out;
    if (!lookup(k, &v))
        return def;
    if (!parse_i64(v, &out)) {
        bad_value(k, v, "an integer");
        return def;
    }
    return out;
}

double Config::get_f64(Str k, double def) {
    Str v;
    double out;
    if (!lookup(k, &v))
        return def;
    if (!parse_f64(v, &out)) {
        bad_value(k, v, "a number");
        return def;
    }
    return out;
}

bool Config::get_bool(Str k, bool def) {
    Str v;
    bool out;
    if (!lookup(k, &v))
        return def;
    if (!parse_bool(v, &out)) {
        bad_value(k, v, "true/false");
        return def;
    }
    return out;
}

Str Config::get_str(Str k, Str def) {
    Str v;
    return lookup(k, &v) ? v : def;
}

void Config::set(Str k, Str v) {
    NOVA_ASSERT(valid_key(k));
    NOVA_ASSERT(v.find('#') == v.n && v.find('\n') == v.n);
    // Copy first: k or v may point into arena_, which store() can reallocate.
    TextBuf kc, vc;
    kc.append(k);
    vc.append(v);
    i64 idx = index_of(kc.view());
    if (idx >= 0) {
        Entry& e = entries_[static_cast<usize>(idx)];
        e.val_off = store(vc.view());
        e.val_len = static_cast<u32>(vc.size());
        return;
    }
    Entry e;
    e.key_off = store(kc.view());
    e.key_len = static_cast<u32>(kc.size());
    e.val_off = store(vc.view());
    e.val_len = static_cast<u32>(vc.size());
    e.line = 0;
    e.used = 0;
    entries_.push(e);
}

bool Config::first_unused(Str* k) const {
    for (usize i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].used) {
            *k = key(i);
            return true;
        }
    }
    return false;
}

void Config::write(TextBuf& out) const {
    for (usize i = 0; i < entries_.size(); ++i) {
        out.append(key(i));
        out.append(" = ");
        out.append(value(i));
        out.append_char('\n');
    }
}

} // namespace nova
