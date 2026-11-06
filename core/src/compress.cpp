#include "nova/compress.h"

#include <cmath> // log2

#include "nova/array.h"

namespace nova {

namespace {

constexpr u32 kMinMatch = 4;     // shorter matches cost more than their literals
constexpr u32 kMaxMatch = 4096;
constexpr u32 kOffsetBits = 16;  // matches reach back up to 64K codons
constexpr u32 kWindow = 1u << kOffsetBits;
constexpr u32 kChain = 32;       // candidates tried per position (bounded search)
constexpr u32 kHashBits = 16;

u32 hash4(const u8* p) {
    u32 v = static_cast<u32>(p[0] & 63) | static_cast<u32>(p[1] & 63) << 6 |
            static_cast<u32>(p[2] & 63) << 12 | static_cast<u32>(p[3] & 63) << 18;
    return (v * 2654435761u) >> (32 - kHashBits);
}

// Elias-gamma code length for x >= 1.
u32 gamma_bits(u32 x) {
    u32 b = 0;
    while ((x >> b) > 1)
        ++b;
    return 2 * b + 1;
}

} // namespace

Compressibility estimate_compressibility(const u8* data, usize n) {
    Compressibility c;
    c.measured = true;
    if (n == 0) {
        c.ratio = 1.0;
        return c;
    }
    Array<i64> head, prev;
    head.resize(static_cast<usize>(1) << kHashBits);
    head.fill(-1);
    prev.resize(n);
    auto insert = [&](usize pos) {
        if (pos + kMinMatch <= n) {
            u32 h = hash4(data + pos);
            prev[pos] = head[h];
            head[h] = static_cast<i64>(pos);
        }
    };

    u64 freq[64] = {};
    double match_bits = 0;
    usize i = 0;
    while (i < n) {
        u32 best = 0;
        if (i + kMinMatch <= n) {
            const u32 maxlen = static_cast<u32>(min_of<usize>(kMaxMatch, n - i));
            i64 cand = head[hash4(data + i)];
            for (u32 tries = 0; cand >= 0 && tries < kChain; ++tries) {
                const usize at = static_cast<usize>(cand);
                if (i - at > kWindow)
                    break;
                u32 l = 0; // the match may overlap position i (at + l < i + l < n)
                while (l < maxlen && (data[at + l] & 63) == (data[i + l] & 63))
                    ++l;
                if (l > best) {
                    best = l;
                    if (l == maxlen)
                        break;
                }
                cand = prev[at];
            }
        }
        if (best >= kMinMatch) {
            ++c.matches;
            match_bits += kOffsetBits + gamma_bits(best - kMinMatch + 1);
            for (u32 j = 0; j < best; ++j)
                insert(i + j);
            i += best;
        } else {
            ++c.literals;
            ++freq[data[i] & 63];
            insert(i);
            ++i;
        }
    }

    double bits = match_bits;
    const double lit = static_cast<double>(c.literals);
    for (u32 s = 0; s < 64; ++s)
        if (freq[s])
            bits += static_cast<double>(freq[s]) * log2(lit / static_cast<double>(freq[s]));
    const double tokens = static_cast<double>(c.matches + c.literals);
    if (c.matches && c.literals) {
        bits += static_cast<double>(c.matches) * log2(tokens / static_cast<double>(c.matches));
        bits += lit * log2(tokens / lit);
    }
    c.ratio = bits / (6.0 * static_cast<double>(n));
    return c;
}

} // namespace nova
