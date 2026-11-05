#include <cstring>

#include "check.h"
#include "nova/config.h"

using namespace nova;

namespace {

bool contains(const char* hay, const char* needle) {
    return strstr(hay, needle) != nullptr;
}

void test_parse_basics() {
    Config c;
    Config::ParseError e;
    const char* text = "# comment line\r\n"
                       "\n"
                       "seed = 42\r\n"
                       "  world.nodes=64   # trailing comment\n"
                       "name = two words here\n"
                       "empty =\n"
                       "rate = 0.25"; // no final newline
    CHECK(c.parse(text, &e));
    CHECK(c.count() == 5);
    CHECK(c.key(0).eq("seed") && c.value(0).eq("42"));
    CHECK(c.value(1).eq("64"));
    CHECK(c.get_str("name", "").eq("two words here"));
    CHECK(c.get_str("empty", "x").empty());
    CHECK(c.get_u64("seed", 0) == 42);
    CHECK(c.get_u64("world.nodes", 0) == 64);
    CHECK(c.get_f64("rate", 0) == 0.25);
    CHECK(c.get_u64("missing", 7) == 7); // default
    CHECK(c.ok());
    CHECK(c.has("seed") && !c.has("missing"));
}

void test_parse_errors() {
    Config c;
    Config::ParseError e;
    CHECK(!c.parse("a = 1\nb = 2\na = 3\n", &e));
    CHECK(e.line == 3);
    CHECK(contains(e.msg, "duplicate key 'a'") && contains(e.msg, "first set on line 1"));
    CHECK(c.count() == 0); // a failed parse leaves the config empty

    CHECK(!c.parse("a = 1\njust words\n", &e));
    CHECK(e.line == 2 && contains(e.msg, "expected 'key = value'"));

    CHECK(!c.parse("bad key = 1\n", &e));
    CHECK(e.line == 1 && contains(e.msg, "invalid key"));

    CHECK(!c.parse("= 1\n", &e));
    CHECK(e.line == 1);

    CHECK(!c.load_file("no/such/file.cfg", &e));
    CHECK(e.line == 0 && contains(e.msg, "cannot read"));
}

void test_typed_getters() {
    Config c;
    Config::ParseError e;
    CHECK(c.parse("n = 12abc\nf = fast\nb = maybe\ni = -5\nok = yes\n", &e));
    CHECK(c.get_i64("i", 0) == -5);
    CHECK(c.get_bool("ok", false));
    CHECK(c.ok());
    CHECK(c.get_u64("n", 3) == 3); // malformed -> default + recorded error
    CHECK(!c.ok());
    CHECK(contains(c.value_error(), "n: expected an unsigned integer, got '12abc'"));
    c.get_f64("f", 0);
    CHECK(contains(c.value_error(), "'12abc'")); // the first problem is kept

    Config d;
    CHECK(d.parse("x = 5\n", &e));
    d.fail("x", "must be even");
    CHECK(contains(d.value_error(), "x: must be even"));
}

void test_unused_detection() {
    Config c;
    Config::ParseError e;
    CHECK(c.parse("world.nodes = 10\nworld.ndoes = 500\n", &e));
    c.get_u64("world.nodes", 64);
    Str k;
    CHECK(c.first_unused(&k) && k.eq("world.ndoes"));
    c.get_u64("world.ndoes", 0);
    CHECK(!c.first_unused(&k));
}

void test_set_and_round_trip() {
    Config c;
    Config::ParseError e;
    CHECK(c.parse("a = 1\nb = two\n", &e));
    c.get_str("a", "");
    c.set("a", "100"); // replace keeps position and used flag
    c.set("c", "0.5"); // insert appends
    CHECK(c.count() == 3);
    CHECK(c.key(0).eq("a") && c.value(0).eq("100"));
    CHECK(c.key(2).eq("c"));
    Str k;
    CHECK(c.first_unused(&k) && k.eq("b"));

    // set() with views into the config's own storage (arena may reallocate).
    for (int i = 0; i < 50; ++i)
        c.set(c.key(1), c.value(1));
    CHECK(c.get_str("b", "").eq("two"));

    TextBuf out;
    c.write(out);
    CHECK(out.view().eq("a = 100\nb = two\nc = 0.5\n"));
    Config d;
    CHECK(d.parse(out.view(), &e));
    bool same = d.count() == c.count();
    for (usize i = 0; same && i < c.count(); ++i)
        same = d.key(i).eq(c.key(i)) && d.value(i).eq(c.value(i));
    CHECK(same);
}

} // namespace

void suite_config() {
    test_parse_basics();
    test_parse_errors();
    test_typed_getters();
    test_unused_detection();
    test_set_and_round_trip();
}
