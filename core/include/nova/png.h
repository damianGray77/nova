#pragma once

// A minimal PNG writer (Phase 6, design.md 6.12): 8-bit RGB, no interlace,
// filter 0 on every row, and the image data in *stored* (uncompressed) deflate
// blocks, so no zlib is needed (design.md 7.1 B). The checksums are hand-rolled:
// CRC-32 for each chunk, Adler-32 for the zlib stream. Files are larger than a
// compressed PNG's but open in any viewer.
#include "nova/io.h"
#include "nova/types.h"

namespace nova {

u32 crc32(const u8* data, usize n, u32 crc = 0);
u32 adler32(const u8* data, usize n, u32 adler = 1);

// Encodes a w x h image whose pixels are 0x00RRGGBB, row by row from the top
// (`stride` pixels apart), as a complete PNG file.
void png_encode(const u32* pixels, u32 w, u32 h, u32 stride, ByteWriter& out);

} // namespace nova
