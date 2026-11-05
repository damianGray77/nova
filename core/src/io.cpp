#include "nova/io.h"

namespace nova {

void ByteWriter::put_u32(u32 v) {
    u8 b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = static_cast<u8>(v >> (8 * i));
    buf_.append(b, 4);
}

void ByteWriter::put_u64(u64 v) {
    u8 b[8];
    for (int i = 0; i < 8; ++i)
        b[i] = static_cast<u8>(v >> (8 * i));
    buf_.append(b, 8);
}

void ByteWriter::put_str(Str s) {
    NOVA_ASSERT(s.n <= 0xFFFFFFFFull);
    put_u32(static_cast<u32>(s.n));
    put_bytes(s.p, s.n);
}

bool ByteReader::take(usize n) {
    if (!ok_ || n > n_ - pos_) {
        ok_ = false;
        return false;
    }
    return true;
}

bool ByteReader::get_u8(u8* v) {
    if (!take(1))
        return false;
    *v = p_[pos_++];
    return true;
}

bool ByteReader::get_u32(u32* v) {
    if (!take(4))
        return false;
    u32 r = 0;
    for (int i = 0; i < 4; ++i)
        r |= static_cast<u32>(p_[pos_ + i]) << (8 * i);
    pos_ += 4;
    *v = r;
    return true;
}

bool ByteReader::get_u64(u64* v) {
    if (!take(8))
        return false;
    u64 r = 0;
    for (int i = 0; i < 8; ++i)
        r |= static_cast<u64>(p_[pos_ + i]) << (8 * i);
    pos_ += 8;
    *v = r;
    return true;
}

bool ByteReader::get_bytes(void* out, usize n) {
    if (!take(n))
        return false;
    if (n)
        memcpy(out, p_ + pos_, n);
    pos_ += n;
    return true;
}

bool ByteReader::get_str(Str* s) {
    u32 len;
    if (!get_u32(&len) || !take(len))
        return false;
    *s = Str(reinterpret_cast<const char*>(p_ + pos_), len);
    pos_ += len;
    return true;
}

bool read_file(const char* path, Array<u8>* out) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    out->clear();
    u8 chunk[65536];
    for (;;) {
        usize got = fread(chunk, 1, sizeof chunk, f);
        out->append(chunk, got);
        if (got < sizeof chunk)
            break;
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

bool write_file(const char* path, const void* data, usize n) {
    FILE* f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

bool TextFile::open(const char* path) {
    close();
    f_ = fopen(path, "wb");
    return f_ != nullptr;
}

void TextFile::write(Str s) {
    if (f_ && s.n)
        fwrite(s.p, 1, s.n, f_);
}

void TextFile::flush() {
    if (f_)
        fflush(f_);
}

void TextFile::close() {
    if (f_) {
        fclose(f_);
        f_ = nullptr;
    }
}

} // namespace nova
