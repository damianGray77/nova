#pragma once

// Soup compressibility estimate (design.md 6.4 #24, 7.1 H; built in Phase 1b).
//
// A hand-rolled greedy LZ77 cost estimate (no zlib). When replicators take
// over, the soup turns repetitive and compresses much better, which is one of
// the two signals that life has appeared (the other is the lineage detector).
//
// The data is six-bit codons. A greedy parse takes, at each position, the
// longest earlier match of at least 4 codons (hash chains, bounded search) or
// else a literal. The estimated size in bits is:
//   - literals at their zero-order entropy (at most 6 bits each);
//   - one match/literal flag per token at the binary entropy of the match rate;
//   - per match, a 16-bit offset plus an Elias-gamma length.
// ratio = estimated bits / (6 x codons): about 1 for uniformly random codons,
// well below 1 for a soup full of copies.
#include "nova/types.h"

namespace nova {

struct Compressibility {
    bool measured = false;
    double ratio = 0.0; // estimated compressed size / raw size; lower = more repetitive
    u64 matches = 0;    // tokens that were matches
    u64 literals = 0;   // tokens that were literals
};

Compressibility estimate_compressibility(const u8* data, usize n);

} // namespace nova
