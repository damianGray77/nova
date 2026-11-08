#pragma once

// Array<T>: Nova's growable vector.
//
// Elements must be trivially copyable. That is deliberate, not a shortcut:
//   * growth is a plain realloc and copies are memcpy, so no placement new and
//     no <new> header are needed (the no-STL line, design.md 7.1 B);
//   * world state built from such arrays can be written to a snapshot byte for
//     byte;
//   * Array<Array<T>> is rejected at compile time, which steers world data into
//     flat arrays plus offsets -- the layout the tick loop wants anyway.
#include <cstdlib> // malloc, realloc, free
#include <cstring> // memcpy, memmove, memset

#include "nova/types.h"

namespace nova {

template <class T> class Array {
    static_assert(__is_trivially_copyable(T),
                  "nova::Array holds trivially copyable types only (flat data, no <new>)");

  public:
    Array() = default;
    ~Array() { free(data_); }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) {
        o.data_ = nullptr;
        o.size_ = o.cap_ = 0;
    }
    Array& operator=(Array&& o) noexcept {
        if (this != &o) {
            free(data_);
            data_ = o.data_;
            size_ = o.size_;
            cap_ = o.cap_;
            o.data_ = nullptr;
            o.size_ = o.cap_ = 0;
        }
        return *this;
    }

    usize size() const { return size_; }
    usize capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }

    T* data() { return data_; }
    const T* data() const { return data_; }
    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    T& operator[](usize i) {
        NOVA_ASSERT(i < size_);
        return data_[i];
    }
    const T& operator[](usize i) const {
        NOVA_ASSERT(i < size_);
        return data_[i];
    }
    T& back() {
        NOVA_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }

    // Guarantees capacity for n elements (never shrinks).
    void reserve(usize n) {
        if (n <= cap_)
            return;
        if (n > ~(usize)0 / sizeof(T))
            NOVA_FATAL("Array::reserve: size overflow");
        T* p = static_cast<T*>(realloc(data_, n * sizeof(T)));
        if (!p)
            NOVA_FATAL("Array::reserve: out of memory");
        data_ = p;
        cap_ = n;
    }

    // Sets the size; new elements are zero-filled.
    void resize(usize n) {
        if (n > cap_)
            reserve(n);
        if (n > size_)
            memset(static_cast<void*>(data_ + size_), 0, (n - size_) * sizeof(T));
        size_ = n;
    }

    // Sets the size, leaving new elements as they are: for a caller that
    // writes every one of them next (growing as push does).
    void resize_unfilled(usize n) {
        if (n > cap_)
            grow_for(n);
        size_ = n;
    }

    void clear() { size_ = 0; }

    void push(const T& v) {
        T copy = v; // v may live inside this array; growing would invalidate it
        if (size_ == cap_)
            grow_for(size_ + 1);
        data_[size_++] = copy;
    }

    void pop() {
        NOVA_ASSERT(size_ > 0);
        --size_;
    }

    // Inserts v before index `at` (at == size() appends).
    void insert(usize at, const T& v) {
        NOVA_ASSERT(at <= size_);
        T copy = v;
        if (size_ == cap_)
            grow_for(size_ + 1);
        memmove(static_cast<void*>(data_ + at + 1), data_ + at, (size_ - at) * sizeof(T));
        data_[at] = copy;
        ++size_;
    }

    void append(const T* src, usize n) {
        if (n == 0)
            return;
        NOVA_ASSERT(src < data_ || src >= data_ + cap_); // no self-append
        if (size_ + n > cap_)
            grow_for(size_ + n);
        memcpy(static_cast<void*>(data_ + size_), src, n * sizeof(T));
        size_ += n;
    }

    void assign(const T* src, usize n) {
        clear();
        append(src, n);
    }

    void fill(const T& v) {
        for (usize i = 0; i < size_; ++i)
            data_[i] = v;
    }

  private:
    void grow_for(usize need) {
        usize cap = cap_ < 8 ? 8 : cap_ * 2;
        reserve(cap < need ? need : cap);
    }

    T* data_ = nullptr;
    usize size_ = 0;
    usize cap_ = 0;
};

// Sorts u64 keys ascending. Because the keys are plain integers, the result is
// fully determined by the input, whichever way it is sorted: insertion sort for
// a few, heapsort (no allocation) up to a thousand or so, and above that an
// LSD radix sort, a byte a pass, skipping bytes every key shares (2026-11-08:
// a mature world's heapsorts of 10^5 keys a tick, on one thread, were
// milliseconds each; a radix pass is a few hundred microseconds).
inline void sort_u64_heap(u64* a, usize n);
inline void sort_u64(u64* a, usize n) {
    if (n < 2)
        return;
    if (n <= 32) {
        for (usize i = 1; i < n; ++i) {
            const u64 x = a[i];
            usize p = i;
            for (; p > 0 && a[p - 1] > x; --p)
                a[p] = a[p - 1];
            a[p] = x;
        }
        return;
    }
    u64* tmp = n >= 1024 ? static_cast<u64*>(malloc(n * sizeof(u64))) : nullptr;
    if (!tmp) {
        sort_u64_heap(a, n);
        return;
    }
    usize count[8][256];
    memset(count, 0, sizeof count);
    for (usize i = 0; i < n; ++i) {
        const u64 x = a[i];
        for (u32 d = 0; d < 8; ++d)
            ++count[d][(x >> (8 * d)) & 255];
    }
    u64* src = a;
    u64* dst = tmp;
    for (u32 d = 0; d < 8; ++d) {
        usize* c = count[d];
        if (c[(src[0] >> (8 * d)) & 255] == n)
            continue; // every key has this byte: the pass would change nothing
        usize sum = 0;
        for (u32 b = 0; b < 256; ++b) {
            const usize k = c[b];
            c[b] = sum;
            sum += k;
        }
        for (usize i = 0; i < n; ++i) {
            const u64 x = src[i];
            dst[c[(x >> (8 * d)) & 255]++] = x;
        }
        u64* t = src;
        src = dst;
        dst = t;
    }
    if (src != a)
        memcpy(a, src, n * sizeof(u64));
    free(tmp);
}

// The in-place heapsort: O(n log n), no allocation.
inline void sort_u64_heap(u64* a, usize n) {
    if (n < 2)
        return;
    auto sift_down = [a](usize root, usize end) {
        for (;;) {
            usize child = 2 * root + 1;
            if (child >= end)
                return;
            if (child + 1 < end && a[child] < a[child + 1])
                ++child;
            if (a[root] >= a[child])
                return;
            u64 t = a[root];
            a[root] = a[child];
            a[child] = t;
            root = child;
        }
    };
    for (usize i = n / 2; i-- > 0;)
        sift_down(i, n);
    for (usize end = n - 1; end > 0; --end) {
        u64 t = a[0];
        a[0] = a[end];
        a[end] = t;
        sift_down(0, end);
    }
}

} // namespace nova
