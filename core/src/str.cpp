#include "nova/str.h"

#include <cstdio>  // snprintf
#include <cstdlib> // strtod

namespace nova {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

Str Str::trim() const {
    usize b = 0, e = n;
    while (b < e && is_space(p[b]))
        ++b;
    while (e > b && is_space(p[e - 1]))
        --e;
    return Str(p + b, e - b);
}

usize Str::find(char c) const {
    for (usize i = 0; i < n; ++i)
        if (p[i] == c)
            return i;
    return n;
}

bool parse_u64(Str s, u64* out) {
    if (s.n == 0)
        return false;
    u64 v = 0;
    if (s.n > 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X')) {
        if (s.n - 2 > 16)
            return false;
        for (usize i = 2; i < s.n; ++i) {
            int d = hex_digit(s.p[i]);
            if (d < 0)
                return false;
            v = (v << 4) | static_cast<u64>(d);
        }
        *out = v;
        return true;
    }
    for (usize i = 0; i < s.n; ++i) {
        char c = s.p[i];
        if (c < '0' || c > '9')
            return false;
        u64 d = static_cast<u64>(c - '0');
        if (v > (~0ull - d) / 10)
            return false; // overflow
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool parse_i64(Str s, i64* out) {
    bool neg = s.n > 0 && s.p[0] == '-';
    u64 mag;
    if (!parse_u64(neg ? s.sub(1, s.n - 1) : s, &mag))
        return false;
    if (neg && s.n > 1 && (s.p[1] == '0' && s.n > 2 && (s.p[2] == 'x' || s.p[2] == 'X')))
        return false; // no negative hex
    if (neg) {
        if (mag > 0x8000000000000000ull)
            return false;
        *out = static_cast<i64>(0 - mag);
    } else {
        if (mag > 0x7FFFFFFFFFFFFFFFull)
            return false;
        *out = static_cast<i64>(mag);
    }
    return true;
}

bool parse_f64(Str s, double* out) {
    // Character filter first: strtod alone would also accept "inf", "nan",
    // hex floats and leading spaces.
    if (s.n == 0 || s.n > 63)
        return false;
    bool digit = false;
    for (usize i = 0; i < s.n; ++i) {
        char c = s.p[i];
        if (c >= '0' && c <= '9')
            digit = true;
        else if (c != '+' && c != '-' && c != '.' && c != 'e' && c != 'E')
            return false;
    }
    if (!digit)
        return false;
    char buf[64];
    memcpy(buf, s.p, s.n);
    buf[s.n] = '\0';
    char* end = nullptr;
    double v = strtod(buf, &end);
    if (end != buf + s.n)
        return false;
    if (v - v != 0.0)
        return false; // overflowed to infinity
    *out = v;
    return true;
}

bool parse_bool(Str s, bool* out) {
    if (s.eq("true") || s.eq("yes") || s.eq("on") || s.eq("1")) {
        *out = true;
        return true;
    }
    if (s.eq("false") || s.eq("no") || s.eq("off") || s.eq("0")) {
        *out = false;
        return true;
    }
    return false;
}

void TextBuf::append_u64(u64 v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%llu", static_cast<unsigned long long>(v));
    buf_.append(tmp, static_cast<usize>(n));
}

void TextBuf::append_i64(i64 v) {
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", static_cast<long long>(v));
    buf_.append(tmp, static_cast<usize>(n));
}

void TextBuf::append_hex64(u64 v) {
    static const char kDigits[] = "0123456789abcdef";
    char tmp[16];
    for (int i = 15; i >= 0; --i) {
        tmp[i] = kDigits[v & 15];
        v >>= 4;
    }
    buf_.append(tmp, 16);
}

void TextBuf::append_f64(double v) {
    char tmp[40];
    int n = 0;
    for (int prec = 15; prec <= 17; ++prec) {
        n = snprintf(tmp, sizeof tmp, "%.*g", prec, v);
        if (strtod(tmp, nullptr) == v)
            break;
    }
    buf_.append(tmp, static_cast<usize>(n));
}

const char* TextBuf::c_str() {
    buf_.reserve(buf_.size() + 1);
    buf_.data()[buf_.size()] = '\0';
    return buf_.data();
}

} // namespace nova
