#include <cstring>

#include "check.h"
#include "nova/str.h"

using namespace nova;

namespace {

void test_views() {
    Str s("  hello world \t\r\n");
    CHECK(s.trim().eq("hello world"));
    CHECK(Str("   ").trim().empty());
    CHECK(Str().trim().empty());
    CHECK(Str("abc").find('c') == 2);
    CHECK(Str("abc").find('z') == 3);
    CHECK(Str("abcdef").sub(2, 3).eq("cde"));
    CHECK(Str("abc").eq(Str("abcd", 3)));
    CHECK(!Str("abc").eq("abd"));
    CHECK(Str("").eq(Str()));
    CHECK(Str("world.nodes").starts_with("world."));
    CHECK(!Str("worl").starts_with("world."));
}

void test_parse_u64() {
    u64 v = 0;
    CHECK(parse_u64("0", &v) && v == 0);
    CHECK(parse_u64("18446744073709551615", &v) && v == ~0ull);
    CHECK(!parse_u64("18446744073709551616", &v)); // overflow
    CHECK(parse_u64("0x2A", &v) && v == 42);
    CHECK(parse_u64("0xffffffffffffffff", &v) && v == ~0ull);
    CHECK(!parse_u64("0x1ffffffffffffffff", &v)); // 17 hex digits
    CHECK(!parse_u64("0x", &v));
    CHECK(!parse_u64("", &v));
    CHECK(!parse_u64("12abc", &v));
    CHECK(!parse_u64("-1", &v));
    CHECK(!parse_u64("+1", &v));
    CHECK(!parse_u64(" 1", &v)); // callers trim; the parser does not
}

void test_parse_i64() {
    i64 v = 0;
    CHECK(parse_i64("-42", &v) && v == -42);
    CHECK(parse_i64("9223372036854775807", &v) && v == 0x7FFFFFFFFFFFFFFFll);
    CHECK(parse_i64("-9223372036854775808", &v) && v == static_cast<i64>(0x8000000000000000ull));
    CHECK(!parse_i64("9223372036854775808", &v));
    CHECK(!parse_i64("-9223372036854775809", &v));
    CHECK(!parse_i64("-", &v));
    CHECK(!parse_i64("--1", &v));
    CHECK(!parse_i64("-0x10", &v));
}

void test_parse_f64_bool() {
    double d = 0;
    CHECK(parse_f64("0.25", &d) && d == 0.25);
    CHECK(parse_f64("-1.5e3", &d) && d == -1500.0);
    CHECK(parse_f64("3", &d) && d == 3.0);
    CHECK(parse_f64(".5", &d) && d == 0.5);
    CHECK(!parse_f64("inf", &d));
    CHECK(!parse_f64("nan", &d));
    CHECK(!parse_f64("0x1p3", &d));
    CHECK(!parse_f64("1e999", &d)); // overflows to infinity
    CHECK(!parse_f64("1.2.3", &d));
    CHECK(!parse_f64("e", &d));
    CHECK(!parse_f64("", &d));

    bool b = false;
    CHECK(parse_bool("true", &b) && b);
    CHECK(parse_bool("off", &b) && !b);
    CHECK(parse_bool("1", &b) && b);
    CHECK(!parse_bool("True", &b));
    CHECK(!parse_bool("2", &b));
}

bool f64_round_trips(double v) {
    TextBuf t;
    t.append_f64(v);
    double back = 0;
    return parse_f64(t.view(), &back) && back == v;
}

void test_textbuf() {
    TextBuf t;
    t.append("x=");
    t.append_u64(18446744073709551615ull);
    t.append_char(' ');
    t.append_i64(-7);
    t.append_char(' ');
    t.append_hex64(0xABCull);
    CHECK(strcmp(t.c_str(), "x=18446744073709551615 -7 0000000000000abc") == 0);
    CHECK(t.size() == strlen(t.c_str())); // the terminator is not counted

    TextBuf f;
    f.append_f64(0.25);
    CHECK(f.view().eq("0.25"));
    f.clear();
    f.append_f64(0.1);
    CHECK(f.view().eq("0.1")); // shortest exact form, not 0.10000000000000001
    CHECK(f64_round_trips(0.1));
    CHECK(f64_round_trips(1.0 / 3.0));
    CHECK(f64_round_trips(1e-300));
    CHECK(f64_round_trips(-2.5));
    CHECK(f64_round_trips(123456789.123456789));
    CHECK(f64_round_trips(0.7));
}

} // namespace

void suite_str() {
    test_views();
    test_parse_u64();
    test_parse_i64();
    test_parse_f64_bool();
    test_textbuf();
}
