#pragma once

// 64-bit mixing and streaming hashes. mix64 is the SplitMix64 finalizer
// (Steele, Lea & Flood 2014; constants from Vigna's reference code). It is a
// bijection on u64 with strong avalanche, and it is the one primitive under the
// RNG, the hash map and the world-state hash.
#include <cstring> // memcpy

#include "nova/types.h"

namespace nova {

inline constexpr u64 kGolden = 0x9E3779B97F4A7C15ull; // 2^64 / golden ratio

constexpr u64 mix64(u64 z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Order-sensitive streaming hash for state fingerprints (world hash, snapshot
// checksum). Not cryptographic: it detects divergence and corruption, it does
// not resist an adversary.
class Hasher {
  public:
    explicit Hasher(u64 domain = 0) : h_(mix64(domain + kGolden)) {}

    void word(u64 w) {
        h_ = mix64((h_ ^ w) + kGolden);
        ++n_;
    }

    // Absorbs bytes 8 at a time (little-endian, as on every Nova target), then a
    // final word holding the 0-7 tail bytes plus the tail length in the top byte,
    // so "ab" and "ab\0" hash differently.
    void bytes(const void* data, usize n) {
        const u8* p = static_cast<const u8*>(data);
        while (n >= 8) {
            u64 w;
            memcpy(&w, p, 8);
            word(w);
            p += 8;
            n -= 8;
        }
        u64 tail = 0;
        for (usize i = 0; i < n; ++i)
            tail |= static_cast<u64>(p[i]) << (8 * i);
        word(tail ^ (static_cast<u64>(n) << 56));
    }

    u64 finish() const { return mix64(h_ ^ (n_ * kGolden)); }

  private:
    u64 h_;
    u64 n_ = 0;
};

inline u64 hash_bytes(const void* data, usize n, u64 domain = 0) {
    Hasher h(domain);
    h.bytes(data, n);
    return h.finish();
}

} // namespace nova
