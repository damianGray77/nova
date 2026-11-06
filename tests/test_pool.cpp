#include "check.h"
#include "nova/array.h"
#include "nova/pool.h"

using namespace nova;

namespace {

// Each task bumps its own slot, so there is no data race even at 20 threads;
// any index run twice or skipped shows up as a count other than 1.
void bump(void* ctx, u32 i) {
    static_cast<u32*>(ctx)[i] += 1;
}

// Uneven work: later indices spin longer, so threads finish out of order.
void bump_uneven(void* ctx, u32 i) {
    volatile u32 spin = 0;
    for (u32 k = 0; k < (i % 17) * 200; ++k)
        spin = spin + 1;
    static_cast<u32*>(ctx)[i] += 1;
}

bool each_once(const Array<u32>& c) {
    for (u32 v : c)
        if (v != 1)
            return false;
    return true;
}

void test_every_index_once() {
    const u32 threads[] = {1, 2, 4, 20};
    const u32 counts[] = {0, 1, 7, 1000, 5000};
    for (u32 t : threads) {
        ThreadPool pool;
        pool.start(t);
        CHECK(pool.threads() == t);
        for (u32 n : counts) {
            Array<u32> c;
            c.resize(n);
            pool.run(n, bump, c.data());
            CHECK(each_once(c));
            c.fill(0);
            pool.run(n, bump_uneven, c.data());
            CHECK(each_once(c));
        }
    }
}

void test_reuse_many_generations() {
    // Thousands of back-to-back runs: catches lost wake-ups or a barrier that
    // returns early (a missed generation would leave a slot at 0).
    ThreadPool pool;
    pool.start(20);
    Array<u32> c;
    c.resize(3);
    bool ok = true;
    for (u32 round = 1; round <= 3000; ++round) {
        pool.run(3, bump, c.data());
        ok = ok && c[0] == round && c[1] == round && c[2] == round;
    }
    CHECK(ok);
}

struct Recorder {
    Array<u32> seen;
};
void record(void* ctx, u32 i) {
    static_cast<Recorder*>(ctx)->seen.push(i);
}

void test_dispatch_order() {
    u32 order[6] = {5, 3, 1, 0, 2, 4};
    ThreadPool pool;
    pool.start(1); // inline: the visit order is observable
    pool.set_dispatch_order(order, 6);
    Recorder r;
    pool.run(6, record, &r);
    bool same = r.seen.size() == 6;
    for (u32 k = 0; same && k < 6; ++k)
        same = r.seen[k] == order[k];
    CHECK(same);
    Recorder r3; // a run of another length goes in order
    pool.run(3, record, &r3);
    CHECK(r3.seen.size() == 3 && r3.seen[0] == 0 && r3.seen[1] == 1 && r3.seen[2] == 2);

    ThreadPool wide;
    wide.start(20);
    wide.set_dispatch_order(order, 6);
    Array<u32> c;
    c.resize(6);
    wide.run(6, bump, c.data());
    CHECK(each_once(c));
}

void test_hardware_threads() {
    CHECK(ThreadPool::hardware_threads() >= 1);
    ThreadPool pool;
    pool.start(0);
    CHECK(pool.threads() == ThreadPool::hardware_threads());
    pool.stop();
    CHECK(pool.threads() == 0);
}

} // namespace

void suite_pool() {
    test_every_index_once();
    test_reuse_many_generations();
    test_dispatch_order();
    test_hardware_threads();
}
