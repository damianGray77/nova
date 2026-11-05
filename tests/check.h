#pragma once

// Minimal hand-rolled test scaffold (itracker's tests/check.h, extended with
// CHECK_EQ_U64 so a mismatching hash prints both values). No framework, no STL;
// output goes through <cstdio> only.
#include <cstdio> // fprintf, printf, stderr

namespace ntest {

inline int checks = 0;
inline int failures = 0;

inline void report(bool ok, const char* expr, const char* file, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        fprintf(stderr, "  FAIL  %s:%d  CHECK(%s)\n", file, line, expr);
    }
}

inline void report_eq(unsigned long long a, unsigned long long b, const char* ea, const char* eb,
                      const char* file, int line) {
    ++checks;
    if (a != b) {
        ++failures;
        fprintf(stderr, "  FAIL  %s:%d  %s == %s\n        %llu (0x%016llx) vs %llu (0x%016llx)\n",
                file, line, ea, eb, a, a, b, b);
    }
}

// Prints a one-line summary and returns a process exit code (0 = all passed).
inline int summary(const char* suite) {
    if (failures == 0) {
        printf("PASS  %s  (%d checks)\n", suite, checks);
        return 0;
    }
    fprintf(stderr, "FAIL  %s  (%d of %d checks failed)\n", suite, failures, checks);
    return 1;
}

} // namespace ntest

// Records a single boolean expectation, capturing its source text and location.
#define CHECK(expr) ::ntest::report((expr), #expr, __FILE__, __LINE__)

// Unsigned equality that prints both sides (decimal and hex) on failure.
#define CHECK_EQ_U64(a, b)                                                                         \
    ::ntest::report_eq(static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), #a, \
                       #b, __FILE__, __LINE__)
