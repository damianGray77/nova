#include "check.h"
#include "nova/array.h"
#include "nova/hashmap.h"
#include "nova/rng.h"

using namespace nova;

namespace {

void test_array_basics() {
    Array<u32> a;
    CHECK(a.empty());
    CHECK(a.data() == nullptr);
    for (u32 i = 0; i < 1000; ++i)
        a.push(i * 3);
    CHECK(a.size() == 1000);
    bool all = true;
    for (u32 i = 0; i < 1000; ++i)
        all = all && a[i] == i * 3;
    CHECK(all);

    a.resize(1010);
    CHECK(a[1005] == 0); // growth zero-fills
    a.resize(10);
    CHECK(a.size() == 10 && a[9] == 27);

    a.insert(0, 7); // [7,0,3,6,9,12,...,27]
    CHECK(a.size() == 11 && a[0] == 7 && a[1] == 0 && a[10] == 27);
    a.insert(a.size(), 99);
    CHECK(a.back() == 99);
    a.insert(5, 55); // [7,0,3,6,9,55,12,...]
    CHECK(a[5] == 55 && a[6] == 12 && a.size() == 13);

    a.pop();
    CHECK(a.size() == 12 && a.back() == 27);

    u32 sum = 0;
    for (u32 v : a)
        sum += v;
    CHECK(sum == 7 + 55 + (0 + 3 + 6 + 9 + 12 + 15 + 18 + 21 + 24 + 27));

    a.fill(5);
    CHECK(a[0] == 5 && a[11] == 5);
}

void test_array_self_push_and_move() {
    // Pushing an element of the array itself while it must grow.
    Array<u64> b;
    b.push(42);
    while (b.size() < b.capacity())
        b.push(1);
    usize before = b.capacity();
    b.push(b[0]);
    CHECK(b.capacity() > before);
    CHECK(b.back() == 42);

    Array<u64> c = nova::move(b);
    CHECK(b.size() == 0 && b.data() == nullptr);
    CHECK(c[0] == 42 && c.back() == 42);

    Array<u64> d;
    d.push(1);
    d = nova::move(c);
    CHECK(d[0] == 42 && c.empty());

    const u64 src[3] = {9, 8, 7};
    d.assign(src, 3);
    CHECK(d.size() == 3 && d[2] == 7);
    d.append(src, 2);
    CHECK(d.size() == 5 && d[4] == 8);
}

bool sorted(const u64* a, usize n) {
    for (usize i = 1; i < n; ++i)
        if (a[i - 1] > a[i])
            return false;
    return true;
}

void test_sort() {
    sort_u64(nullptr, 0);
    u64 one[1] = {5};
    sort_u64(one, 1);
    CHECK(one[0] == 5);

    Rng r(1, 2, 3);
    Array<u64> v;
    u64 sum = 0, x = 0;
    for (int i = 0; i < 5000; ++i) {
        u64 k = r.below(700); // plenty of duplicates
        v.push(k);
        sum += k;
        x ^= k * kGolden;
    }
    sort_u64(v.data(), v.size());
    CHECK(sorted(v.data(), v.size()));
    u64 sum2 = 0, x2 = 0;
    for (u64 k : v) {
        sum2 += k;
        x2 ^= k * kGolden;
    }
    CHECK(sum == sum2 && x == x2); // same multiset

    for (usize i = 0; i < v.size(); ++i) // reversed input
        v[i] = v.size() - i;
    sort_u64(v.data(), v.size());
    CHECK(sorted(v.data(), v.size()) && v[0] == 1);

    u64 big[4] = {~0ull, 0, ~0ull - 1, 1};
    sort_u64(big, 4);
    CHECK(big[0] == 0 && big[1] == 1 && big[2] == ~0ull - 1 && big[3] == ~0ull);
}

void test_hashmap() {
    HashMap<u32> m;
    CHECK(m.find(0) == nullptr);
    CHECK(!m.erase(5));

    bool inserted = false;
    m.get_or_insert(0, &inserted) = 100; // key 0 is an ordinary key
    CHECK(inserted);
    m.get_or_insert(0, &inserted) += 1;
    CHECK(!inserted && *m.find(0) == 101);

    const u32 n = 20000;
    for (u32 i = 1; i <= n; ++i)
        m.get_or_insert(static_cast<u64>(i) * 7919) = i;
    CHECK(m.size() == n + 1);
    bool all = true;
    for (u32 i = 1; i <= n; ++i) {
        const u32* v = m.find(static_cast<u64>(i) * 7919);
        all = all && v && *v == i;
    }
    CHECK(all);

    // Erase every third key; backward-shift must keep every survivor reachable.
    for (u32 i = 3; i <= n; i += 3)
        CHECK(m.erase(static_cast<u64>(i) * 7919));
    bool survivors = true, gone = true;
    for (u32 i = 1; i <= n; ++i) {
        const u32* v = m.find(static_cast<u64>(i) * 7919);
        if (i % 3 == 0)
            gone = gone && v == nullptr;
        else
            survivors = survivors && v && *v == i;
    }
    CHECK(survivors && gone);
    CHECK(m.size() == n + 1 - n / 3);

    usize seen = 0;
    for (usize s = 0; s < m.slot_count(); ++s)
        if (m.slot_used(s))
            ++seen;
    CHECK(seen == m.size());

    m.clear();
    CHECK(m.size() == 0 && m.find(7919) == nullptr);
    m.get_or_insert(7919) = 3;
    CHECK(*m.find(7919) == 3);
}

void test_hashmap_deterministic_layout() {
    HashMap<u32> a, b;
    for (u32 i = 0; i < 3000; ++i) {
        a.get_or_insert(i * 31ull) = i;
        b.get_or_insert(i * 31ull) = i;
        if (i % 5 == 0) {
            a.erase(i * 31ull / 2);
            b.erase(i * 31ull / 2);
        }
    }
    bool same = a.slot_count() == b.slot_count();
    for (usize s = 0; same && s < a.slot_count(); ++s)
        same =
            a.slot_used(s) == b.slot_used(s) && (!a.slot_used(s) || a.slot_key(s) == b.slot_key(s));
    CHECK(same);
}

} // namespace

void suite_containers() {
    test_array_basics();
    test_array_self_push_and_move();
    test_sort();
    test_hashmap();
    test_hashmap_deterministic_layout();
}
