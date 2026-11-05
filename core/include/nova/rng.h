#pragma once

// Counter-based RNG (design.md 7.1 D).
//
// Every random draw is a pure function of (world seed, node id, tick, event
// counter). There is no shared generator, so the order in which threads run
// nodes cannot change any draw -- the foundation of the "same seed, same world
// at any thread count" guarantee. It also means a snapshot never has to save
// RNG state: seed + tick rebuild every stream.
//
// A stream's base is a mix of (seed, node, tick); draw k (k = 0, 1, ...) is
// mix64(base + (k + 1) * golden). That is exactly a SplitMix64 sequence seeded
// with `base`, so the well-studied SplitMix64 statistics apply, and any draw can
// be computed directly from its counter with at().
#include <intrin.h> // _umul128

#include "nova/hash.h"
#include "nova/types.h"

namespace nova {

class Rng {
  public:
    // Reserved stream keys.
    static constexpr u32 kWorldNode = 0xFFFFFFFFu; // world-level generation (the graph)
    static constexpr u64 kInitTick = ~0ull;        // per-node initial state, "before tick 0"

    Rng(u64 seed, u32 node, u64 tick) : base_(stream_base(seed, node, tick)) {}

    // A raw SplitMix64 stream with the given state (used by the known-answer test).
    static Rng from_base(u64 base) {
        Rng r;
        r.base_ = base;
        return r;
    }

    static u64 stream_base(u64 seed, u32 node, u64 tick) {
        u64 h = mix64(seed + kGolden);
        h = mix64(h ^ (0xA24BAED4963EE407ull + node));
        return mix64(h ^ (0x9FB21C651E98DF25ull + tick));
    }

    u64 next() {
        ++counter_;
        return mix64(base_ + counter_ * kGolden);
    }

    // Draw k of this stream, without touching the counter.
    u64 at(u64 k) const { return mix64(base_ + (k + 1) * kGolden); }

    // How many draws have been taken. Saving and restoring it continues a stream
    // exactly (the barrier phase continues each node's stream this way).
    u64 counter() const { return counter_; }
    void set_counter(u64 c) { counter_ = c; }

    // Uniform in [0, n), unbiased (Lemire's multiply-and-reject). n must be > 0.
    u64 below(u64 n) {
        NOVA_ASSERT(n > 0);
        u64 hi;
        u64 lo = _umul128(next(), n, &hi);
        if (lo < n) {
            u64 threshold = (0 - n) % n;
            while (lo < threshold)
                lo = _umul128(next(), n, &hi);
        }
        return hi;
    }

    // Uniform in [0, 1) with 53 bits of precision.
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    bool chance(double p) { return unit() < p; }

  private:
    Rng() = default;

    u64 base_ = 0;
    u64 counter_ = 0;
};

} // namespace nova
