#pragma once

// Soup compressibility estimate (design.md 6.4 #24, 7.1 H) -- Phase 0 STUB.
//
// The real version is a hand-rolled LZ-style estimate (no zlib): when
// replicators take over, the soup turns repetitive and compresses much better,
// which is one of the two signals that life has appeared. It lands in Phase 1b,
// when there is a soup worth measuring. Until then the interface is fixed and
// the result says plainly that nothing was measured, so no stat can mistake a
// placeholder for a reading.
#include "nova/types.h"

namespace nova {

struct Compressibility {
    bool measured = false;
    double ratio = 0.0; // estimated compressed size / raw size; lower = more repetitive
};

Compressibility estimate_compressibility(const u8* data, usize n);

} // namespace nova
