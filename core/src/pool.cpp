#include "nova/pool.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib> // calloc, free

namespace nova {

double clock_seconds() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

namespace {

struct PoolImpl {
    SRWLOCK lock;
    CONDITION_VARIABLE wake; // workers wait here for a new generation (or quit)
    CONDITION_VARIABLE done; // the caller waits here for busy == 0

    // Guarded by lock.
    u64 generation;
    u32 busy; // workers that have not finished the current generation
    bool quit;

    // The current job. Written by the caller under lock before the generation
    // bump; read by workers under lock after they see the bump.
    ThreadPool::TaskFn fn;
    void* ctx;
    u32 count;
    const u32* order;

    volatile LONG next; // the shared queue head (interlocked)

    HANDLE* handles;
    u32 workers;
};

// Pull indices until the range is exhausted. Shared by workers and the caller.
void drain(PoolImpl* p, ThreadPool::TaskFn fn, void* ctx, u32 count, const u32* order) {
    for (;;) {
        LONG slot = InterlockedIncrement(&p->next) - 1;
        if (slot < 0 || static_cast<u32>(slot) >= count)
            return;
        u32 k = static_cast<u32>(slot);
        fn(ctx, order ? order[k] : k);
    }
}

DWORD WINAPI worker_main(LPVOID arg) {
    PoolImpl* p = static_cast<PoolImpl*>(arg);
    u64 seen = 0;
    for (;;) {
        AcquireSRWLockExclusive(&p->lock);
        while (p->generation == seen && !p->quit)
            SleepConditionVariableSRW(&p->wake, &p->lock, INFINITE, 0);
        if (p->quit) {
            ReleaseSRWLockExclusive(&p->lock);
            return 0;
        }
        seen = p->generation;
        ThreadPool::TaskFn fn = p->fn;
        void* ctx = p->ctx;
        u32 count = p->count;
        const u32* order = p->order;
        ReleaseSRWLockExclusive(&p->lock);

        drain(p, fn, ctx, count, order);

        AcquireSRWLockExclusive(&p->lock);
        if (--p->busy == 0)
            WakeConditionVariable(&p->done);
        ReleaseSRWLockExclusive(&p->lock);
    }
}

} // namespace

u32 ThreadPool::hardware_threads() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? static_cast<u32>(n) : 1;
}

void ThreadPool::start(u32 threads) {
    stop();
    if (threads == 0)
        threads = hardware_threads();
    if (threads > 1024)
        threads = 1024;
    threads_ = threads;
    u32 workers = threads - 1;
    if (workers == 0)
        return; // inline mode: no impl, no threads

    PoolImpl* p = static_cast<PoolImpl*>(calloc(1, sizeof(PoolImpl)));
    if (!p)
        NOVA_FATAL("ThreadPool: out of memory");
    InitializeSRWLock(&p->lock);
    InitializeConditionVariable(&p->wake);
    InitializeConditionVariable(&p->done);
    p->handles = static_cast<HANDLE*>(calloc(workers, sizeof(HANDLE)));
    if (!p->handles)
        NOVA_FATAL("ThreadPool: out of memory");
    for (u32 i = 0; i < workers; ++i) {
        p->handles[i] = CreateThread(nullptr, 0, worker_main, p, 0, nullptr);
        if (!p->handles[i])
            NOVA_FATAL("ThreadPool: CreateThread failed");
        ++p->workers;
    }
    impl_ = p;
}

void ThreadPool::stop() {
    PoolImpl* p = static_cast<PoolImpl*>(impl_);
    if (p) {
        AcquireSRWLockExclusive(&p->lock);
        p->quit = true;
        ReleaseSRWLockExclusive(&p->lock);
        WakeAllConditionVariable(&p->wake);
        for (u32 i = 0; i < p->workers; ++i) {
            WaitForSingleObject(p->handles[i], INFINITE);
            CloseHandle(p->handles[i]);
        }
        free(p->handles);
        free(p);
        impl_ = nullptr;
    }
    threads_ = 0;
}

void ThreadPool::run(u32 count, TaskFn fn, void* ctx) {
    NOVA_ASSERT(threads_ > 0); // start() first
    if (count == 0)
        return;
    NOVA_ASSERT(count <= 0x7FFFFFFFu);
    // The test order is a permutation of [0, order_count_): runs of another
    // length (Phase 8's copying a job at a time) go in order.
    const u32* order = count == order_count_ ? order_ : nullptr;
    PoolImpl* p = static_cast<PoolImpl*>(impl_);
    if (!p) {
        for (u32 k = 0; k < count; ++k)
            fn(ctx, order ? order[k] : k);
        return;
    }

    AcquireSRWLockExclusive(&p->lock);
    p->fn = fn;
    p->ctx = ctx;
    p->count = count;
    p->order = order;
    p->next = 0;
    p->busy = p->workers;
    ++p->generation;
    ReleaseSRWLockExclusive(&p->lock);
    WakeAllConditionVariable(&p->wake);

    drain(p, fn, ctx, count, order); // the caller is one of the threads

    // Barrier: wait until every worker has finished this generation. The lock
    // hand-off also makes all task writes visible to the caller.
    AcquireSRWLockExclusive(&p->lock);
    while (p->busy != 0)
        SleepConditionVariableSRW(&p->done, &p->lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&p->lock);
}

} // namespace nova
