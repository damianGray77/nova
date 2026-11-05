#include "check.h"
#include "nova/hash.h"
#include "nova/rng.h"

using namespace nova;

namespace {

// The published SplitMix64 reference sequence (Vigna's splitmix64.c, seed
// 1234567). Pins mix64 and the stream step to the real algorithm.
void test_splitmix_known_answers() {
    Rng r = Rng::from_base(1234567);
    CHECK_EQ_U64(r.next(), 6457827717110365317ull);
    CHECK_EQ_U64(r.next(), 3203168211198807973ull);
    CHECK_EQ_U64(r.next(), 9817491932198370423ull);
    CHECK_EQ_U64(r.next(), 4593380528125082431ull);
    CHECK_EQ_U64(r.next(), 16408922859458223821ull);
}

// Golden values for Nova's own stream keying, computed independently with
// BigInteger arithmetic in PowerShell. If these change, every seed produces a
// different world: that is a format break, never a refactor.
void test_nova_stream_golden() {
    CHECK_EQ_U64(Rng::stream_base(42, 0, 0), 0x2c5da8bf94ee2e0cull);
    Rng r(42, 0, 0);
    CHECK_EQ_U64(r.next(), 0x7787c154ee03d736ull);
    CHECK_EQ_U64(r.next(), 0x0a54f4ad6f213217ull);
    CHECK_EQ_U64(r.next(), 0xdf9d74c8908d2781ull);
    CHECK_EQ_U64(Rng(42, 7, 1000).next(), 0xebf38c33f6a10aa2ull);
    CHECK_EQ_U64(Rng(42, Rng::kWorldNode, 0).next(), 0x97ab53259a032c04ull);
}

void test_counter_access() {
    Rng a(9, 3, 77);
    u64 draws[10];
    for (u64& d : draws)
        d = a.next();
    CHECK(a.counter() == 10);
    Rng b(9, 3, 77);
    bool same = true;
    for (u64 k = 0; k < 10; ++k)
        same = same && b.at(k) == draws[k];
    CHECK(same);
    CHECK(b.counter() == 0); // at() does not advance

    Rng c(9, 3, 77);
    c.set_counter(6); // continue a stream mid-way (the barrier phase does this)
    CHECK(c.next() == draws[6]);
}

void test_streams_differ() {
    u64 base = Rng(42, 0, 0).next();
    CHECK(Rng(43, 0, 0).next() != base);
    CHECK(Rng(42, 1, 0).next() != base);
    CHECK(Rng(42, 0, 1).next() != base);
    CHECK(Rng(42, 0, Rng::kInitTick).next() != base);
    // Swapping node and tick must not alias.
    CHECK(Rng(42, 5, 9).next() != Rng(42, 9, 5).next());
}

void test_bounded() {
    Rng r(1, 1, 1);
    CHECK(r.below(1) == 0);
    bool in_range = true;
    for (int i = 0; i < 10000; ++i) {
        in_range = in_range && r.below(1000) < 1000;
        in_range = in_range && r.below(0x8000000000000001ull) < 0x8000000000000001ull;
    }
    CHECK(in_range);

    // Coarse uniformity: 60,000 draws over 6 buckets, expect 10,000 each
    // (sd ~91). The draws are seeded, so this can never flake.
    u32 bucket[6] = {};
    for (int i = 0; i < 60000; ++i)
        ++bucket[r.below(6)];
    bool uniform = true;
    for (u32 b : bucket)
        uniform = uniform && b > 9500 && b < 10500;
    CHECK(uniform);

    double lo = 1, hi = 0, sum = 0;
    for (int i = 0; i < 100000; ++i) {
        double u = r.unit();
        lo = u < lo ? u : lo;
        hi = u > hi ? u : hi;
        sum += u;
    }
    CHECK(lo >= 0.0 && hi < 1.0);
    CHECK(sum / 100000 > 0.49 && sum / 100000 < 0.51);

    u32 yes = 0;
    for (int i = 0; i < 100000; ++i)
        yes += r.chance(0.25) ? 1 : 0;
    CHECK(yes > 24000 && yes < 26000);
    CHECK(!r.chance(0.0));
}

void test_hasher() {
    Hasher a, b;
    a.word(1);
    a.word(2);
    b.word(2);
    b.word(1);
    CHECK(a.finish() != b.finish()); // order-sensitive

    CHECK(hash_bytes("ab", 2) != hash_bytes("ab\0", 3)); // tail length counts
    CHECK(hash_bytes("abcdefgh", 8) != hash_bytes("abcdefg", 7));
    CHECK(hash_bytes("abc", 3, 1) != hash_bytes("abc", 3, 2)); // domains separate
    CHECK(hash_bytes("abc", 3) == hash_bytes("abc", 3));
    CHECK(Hasher().finish() != Hasher(1).finish());
    CHECK(mix64(0) == 0); // known fixed point of the finalizer -- why the Hasher adds golden
}

} // namespace

void suite_rng() {
    test_splitmix_known_answers();
    test_nova_stream_golden();
    test_counter_access();
    test_streams_differ();
    test_bounded();
    test_hasher();
}
