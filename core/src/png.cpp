#include "nova/png.h"

namespace nova {

namespace {

// The CRC-32 table (polynomial 0xEDB88320, as zlib and PNG use), built once.
struct CrcTable {
    u32 t[256];
    CrcTable() {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    }
};

const CrcTable& crc_table() {
    static const CrcTable table;
    return table;
}

void put_be32(Array<u8>& out, u32 v) {
    const u8 b[4] = {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8),
                     static_cast<u8>(v)};
    out.append(b, 4);
}

// One chunk: length, type, data, CRC over type and data.
void chunk(ByteWriter& out, const char* type, const u8* data, usize n) {
    Array<u8> c;
    put_be32(c, static_cast<u32>(n));
    c.append(reinterpret_cast<const u8*>(type), 4);
    c.append(data, n);
    const u32 crc = crc32(c.data() + 4, c.size() - 4);
    put_be32(c, crc);
    out.put_bytes(c.data(), c.size());
}

} // namespace

u32 crc32(const u8* data, usize n, u32 crc) {
    const u32* t = crc_table().t;
    crc = ~crc;
    for (usize i = 0; i < n; ++i)
        crc = t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

u32 adler32(const u8* data, usize n, u32 adler) {
    u32 a = adler & 0xFFFF, b = adler >> 16;
    while (n > 0) {
        usize k = n < 5552 ? n : 5552; // the most bytes before the sums can overflow
        n -= k;
        while (k--) {
            a += *data++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

void png_encode(const u32* pixels, u32 w, u32 h, u32 stride, ByteWriter& out) {
    static const u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.put_bytes(kSignature, 8);

    Array<u8> ihdr;
    put_be32(ihdr, w);
    put_be32(ihdr, h);
    const u8 rest[5] = {8, 2, 0, 0, 0}; // 8 bits, RGB, deflate, filter method 0, no interlace
    ihdr.append(rest, 5);
    chunk(out, "IHDR", ihdr.data(), ihdr.size());

    // The raw scanlines: a filter byte (0, none), then RGB.
    Array<u8> raw;
    raw.reserve(static_cast<usize>(h) * (1 + 3 * static_cast<usize>(w)));
    for (u32 y = 0; y < h; ++y) {
        raw.push(0);
        const u32* row = pixels + static_cast<usize>(y) * stride;
        for (u32 x = 0; x < w; ++x) {
            const u8 px[3] = {static_cast<u8>(row[x] >> 16), static_cast<u8>(row[x] >> 8), static_cast<u8>(row[x])};
            raw.append(px, 3);
        }
    }

    // zlib: header 0x78 0x01 (deflate, 32K window, fastest; 0x7801 % 31 == 0),
    // stored blocks of at most 65,535 bytes, then the Adler-32 of the raw data.
    Array<u8> z;
    const u8 zhead[2] = {0x78, 0x01};
    z.append(zhead, 2);
    usize at = 0;
    do {
        const usize left = raw.size() - at;
        const u32 len = static_cast<u32>(left < 65535 ? left : 65535);
        const bool last = at + len == raw.size();
        const u8 head[5] = {static_cast<u8>(last ? 1 : 0), static_cast<u8>(len), static_cast<u8>(len >> 8),
                            static_cast<u8>(~len), static_cast<u8>((~len) >> 8)};
        z.append(head, 5);
        z.append(raw.data() + at, len);
        at += len;
    } while (at < raw.size());
    put_be32(z, adler32(raw.data(), raw.size()));
    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);
}

} // namespace nova
