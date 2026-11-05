#pragma once

// Str (a non-owning char view), strict number parsing, and TextBuf (a growable
// text builder). Nova's replacement for the slice of std::string it would need.
#include <cstring> // strlen, memcmp

#include "nova/array.h"
#include "nova/types.h"

namespace nova {

// View of a char range. Not NUL-terminated; never owns memory.
struct Str {
    const char* p = nullptr;
    usize n = 0;

    constexpr Str() = default;
    constexpr Str(const char* s, usize len) : p(s), n(len) {}
    // Implicit, so string literals pass straight to Str parameters.
    Str(const char* s) : p(s), n(s ? strlen(s) : 0) {}

    bool empty() const { return n == 0; }
    char operator[](usize i) const {
        NOVA_ASSERT(i < n);
        return p[i];
    }
    bool eq(Str o) const { return n == o.n && (n == 0 || memcmp(p, o.p, n) == 0); }

    Str sub(usize from, usize len) const {
        NOVA_ASSERT(from <= n && len <= n - from);
        return Str(p + from, len);
    }
    Str trim() const;         // strips spaces, tabs, CR and LF at both ends
    usize find(char c) const; // index of the first c, or n if absent
    bool starts_with(Str prefix) const {
        return prefix.n <= n && (prefix.n == 0 || memcmp(p, prefix.p, prefix.n) == 0);
    }
};

// Strict parsers: the whole view must be a valid number, nothing else. They are
// locale-independent in practice (Nova never calls setlocale, so the C locale
// and its '.' decimal point always apply).
bool parse_u64(Str s, u64* out);    // decimal or 0x-hex; no sign; overflow rejected
bool parse_i64(Str s, i64* out);    // decimal with optional leading '-'
bool parse_f64(Str s, double* out); // digits, sign, '.', exponent; no inf/nan/hex floats
bool parse_bool(Str s, bool* out);  // true/false, yes/no, on/off, 1/0

// Growable text. c_str() NUL-terminates without counting the terminator.
class TextBuf {
  public:
    void append(Str s) { buf_.append(s.p, s.n); }
    void append_char(char c) { buf_.push(c); }
    void append_u64(u64 v);
    void append_i64(i64 v);
    void append_hex64(u64 v); // exactly 16 lowercase hex digits
    // Shortest of %.15g / %.16g / %.17g that reads back to the identical double,
    // so values written to configs and logs round-trip exactly.
    void append_f64(double v);

    Str view() const { return Str(buf_.data(), buf_.size()); }
    const char* c_str();
    usize size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

  private:
    Array<char> buf_;
};

} // namespace nova
