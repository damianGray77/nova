#pragma once

// Shared helpers for the interpreter and soup suites: params from config text,
// a tiny assembler (Op names -> codons) and planting a program as a body.
#include <cstdio>

#include "nova/soup.h"

// Heat and every hazard off: the deterministic baseline most unit tests use.
inline const char* const kQuiet = "heat.enabled = false\n"
                                  "hazard.copy = 0\n"
                                  "hazard.flaw = 0\n"
                                  "hazard.rot = 0\n";

// Builds soup params from config text for a soup of `size` codons. Fails (and
// says why) on a bad value or a key nobody read.
inline bool make_params(nova::SoupParams* p, const char* text, nova::u32 size) {
    nova::Config cfg;
    nova::Config::ParseError pe;
    if (!cfg.parse(text, &pe)) {
        fprintf(stderr, "  make_params: line %u: %s\n", pe.line, pe.msg);
        return false;
    }
    if (!nova::soup_params_from_config(cfg, size, p)) {
        fprintf(stderr, "  make_params: %s\n", cfg.value_error());
        return false;
    }
    nova::Str unused;
    if (cfg.first_unused(&unused)) {
        fprintf(stderr, "  make_params: unused key %.*s\n", static_cast<int>(unused.n), unused.p);
        return false;
    }
    return true;
}

// Writes the program into memory at `at` (which must be waste) and makes it a
// body with `units` millicores. Returns the body slot, or kNone.
inline nova::u32 plant(nova::Soup& s, nova::u32 at, const char* names, nova::u32 units) {
    nova::Array<nova::u8> code;
    nova::TextBuf err;
    if (!nova::genome_from_names(s.params().table, names, &code, &err)) {
        fprintf(stderr, "  plant: %s\n", err.c_str());
        return nova::kNone;
    }
    for (nova::usize i = 0; i < code.size(); ++i)
        s.mem_mut()[at + i] = code[i];
    return s.create_body(at, static_cast<nova::u32>(code.size()), units);
}

// Runs the body's first CPU for exactly n instructions (one at a time).
inline void steps(nova::Soup& s, nova::u32 body, nova::u32 n) {
    nova::u32 c = s.body(body).cpus;
    for (nova::u32 i = 0; i < n; ++i)
        s.run_cpu(c, 1);
}

inline bool soup_ok(const nova::Soup& s) {
    nova::TextBuf why;
    if (s.check(&why))
        return true;
    fprintf(stderr, "  soup invariant: %s\n", why.c_str());
    return false;
}
