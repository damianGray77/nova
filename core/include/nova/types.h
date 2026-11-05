#pragma once

// Fixed-width integer aliases, the two failure macros, and the tiny subset of
// <utility> Nova needs. Thin C-runtime headers only -- no STL (design.md 7.1 B).
#include <cstddef> // size_t
#include <cstdint> // uint8_t .. int64_t

namespace nova {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;
using usize = size_t;

// Prints "nova: fatal: <msg> (<file>:<line>)" to stderr and ends the process.
// Reserved for broken invariants and out-of-memory. Bad *input* (config, files)
// is reported through return values so tests can exercise it.
[[noreturn]] void fatal(const char* file, int line, const char* msg);

template <class T> struct remove_ref {
    using type = T;
};
template <class T> struct remove_ref<T&> {
    using type = T;
};
template <class T> struct remove_ref<T&&> {
    using type = T;
};

// Equivalent of std::move: casts to an rvalue so move constructors are chosen.
template <class T> constexpr typename remove_ref<T>::type&& move(T&& v) noexcept {
    return static_cast<typename remove_ref<T>::type&&>(v);
}

// Named min_of/max_of so windows.h's min/max macros can never collide.
template <class T> constexpr T min_of(T a, T b) {
    return a < b ? a : b;
}
template <class T> constexpr T max_of(T a, T b) {
    return a < b ? b : a;
}

} // namespace nova

#define NOVA_FATAL(msg) ::nova::fatal(__FILE__, __LINE__, (msg))

#ifdef NDEBUG
#define NOVA_ASSERT(expr) ((void)0)
#else
#define NOVA_ASSERT(expr)                                                                          \
    ((expr) ? (void)0 : ::nova::fatal(__FILE__, __LINE__, "assertion failed: " #expr))
#endif
