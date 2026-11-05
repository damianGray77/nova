#pragma once

// ThreadPool: the per-tick shared task queue (design.md 7.1 E).
//
// run(count, fn, ctx) calls fn(ctx, i) once for every i in [0, count). The
// calling thread and N-1 workers all pull the next index from one shared
// interlocked counter until the range is exhausted, and run() returns only when
// every call has finished -- that return is the tick barrier.
//
// Why a shared counter instead of static regions: the i7-12700H mixes 6
// performance and 8 efficiency cores, so fixed slices would leave the fast cores
// idle waiting on the slow ones. Pulling one index at a time self-balances.
//
// Nothing about *which* thread runs a task, or when, may affect results; the
// world guarantees that by having each task touch only its own node's state.
// set_dispatch_order() exists so tests can prove it by permuting the order.
//
// Win32 primitives only (threads, SRWLOCK, CONDITION_VARIABLE, Interlocked*):
// no <thread>, <mutex> or <atomic>. windows.h stays out of this header.
#include "nova/types.h"

namespace nova {

class ThreadPool {
  public:
    using TaskFn = void (*)(void* ctx, u32 index);

    ThreadPool() = default;
    ~ThreadPool() { stop(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // threads = total threads including the caller; 0 = every hardware thread.
    // threads = 1 runs tasks inline on the caller with no worker threads.
    void start(u32 threads);
    void stop();

    u32 threads() const { return threads_; }
    void run(u32 count, TaskFn fn, void* ctx);

    // Test hook: slot k of the shared queue runs task order[k] instead of k.
    // order must be a permutation of [0, count) for every later run() and must
    // outlive them; nullptr restores the identity order.
    void set_dispatch_order(const u32* order) { order_ = order; }

    static u32 hardware_threads();

  private:
    void* impl_ = nullptr; // PoolImpl, defined in pool.cpp
    u32 threads_ = 0;
    const u32* order_ = nullptr;
};

} // namespace nova
