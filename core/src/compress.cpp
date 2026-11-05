#include "nova/compress.h"

namespace nova {

Compressibility estimate_compressibility(const u8* data, usize n) {
    (void)data;
    (void)n;
    return Compressibility{}; // Phase 1b: LZ-style estimate
}

} // namespace nova
