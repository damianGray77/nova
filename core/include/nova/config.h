#pragma once

// Hand-parsed `key = value` configuration (design.md 7.1 B: no JSON library).
//
//   # full-line comment
//   seed = 42              # trailing comments are allowed too
//   world.nodes = 64
//
// Rules: one entry per line; keys are [A-Za-z0-9_.-]+ (dotted namespaces by
// convention); values are the trimmed text after '=' (they may be empty and may
// not contain '#'); a duplicate key or a line without '=' is an error that names
// the line. CRLF and LF both work.
//
// Typo safety: every typed getter marks its key as used, and first_unused()
// reports any key nobody read -- so `world.ndoes = 500` fails loudly instead of
// silently running with the default node count.
#include "nova/array.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

class Config {
  public:
    struct ParseError {
        u32 line = 0; // 1-based; 0 = not tied to a line (e.g. file not found)
        char msg[192] = {};
    };

    // Replaces the contents. On failure the config is left empty.
    bool parse(Str text, ParseError* err);
    bool load_file(const char* path, ParseError* err);
    void clear();

    bool has(Str key) const { return index_of(key) >= 0; }

    // Typed getters. Absent key -> def. Present but malformed -> def, and the
    // first such problem is kept in value_error() (ok() turns false).
    u64 get_u64(Str key, u64 def);
    i64 get_i64(Str key, i64 def);
    double get_f64(Str key, double def);
    bool get_bool(Str key, bool def);
    Str get_str(Str key, Str def); // view valid until the next set()/parse()

    // Lets callers report range checks through the same channel.
    void fail(Str key, const char* what);
    bool ok() const { return value_error_[0] == '\0'; }
    const char* value_error() const { return value_error_; }

    // Inserts or replaces (a replaced key keeps its position and used flag).
    // Used for mid-run knob changes so the effective config is always current.
    void set(Str key, Str value);

    usize count() const { return entries_.size(); }
    Str key(usize i) const;
    Str value(usize i) const;
    bool first_unused(Str* key) const;

    // Writes "key = value\n" per entry, in insertion order. parse(write()) gives
    // back the same entries.
    void write(TextBuf& out) const;

    static bool valid_key(Str key);

    // Fallback defaults (Phase 10): when set, a key a config lacks takes its
    // value from this text before the getter's own default (and needn't be
    // read). Process-wide and set once: the test runner sets the defaults from
    // before Phase 10, which turned the world's features on by default, so its
    // suites keep testing what they test. A real run never sets it. nullptr
    // clears it. False (with the reason) if the text doesn't parse.
    static bool set_fallback(const char* text, ParseError* err);

  private:
    struct Entry {
        u32 key_off, key_len;
        u32 val_off, val_len;
        u32 line;
        u32 used;
    };

    i64 index_of(Str key) const;
    u32 store(Str s);
    bool lookup(Str key, Str* value);
    void bad_value(Str key, Str value, const char* expected);

    Array<char> arena_; // all key and value text, appended
    Array<Entry> entries_;
    char value_error_[192] = {};
};

} // namespace nova
