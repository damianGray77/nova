#pragma once

// Byte-level encoding (explicit little-endian, independent of host layout) and
// whole-file helpers. Snapshots use ByteWriter/ByteReader; CSV and the replay
// log use TextFile.
#include <cstdio> // FILE

#include "nova/array.h"
#include "nova/str.h"
#include "nova/types.h"

namespace nova {

class ByteWriter {
  public:
    void put_u8(u8 v) { buf_.push(v); }
    void put_u32(u32 v);
    void put_u64(u64 v);
    void put_bytes(const void* p, usize n) { buf_.append(static_cast<const u8*>(p), n); }
    void put_str(Str s); // u32 length + bytes

    const u8* data() const { return buf_.data(); }
    usize size() const { return buf_.size(); }

  private:
    Array<u8> buf_;
};

// Bounds-checked reader. Any overrun makes every later get_* fail too (sticky),
// so callers can read a whole section and check ok() once.
class ByteReader {
  public:
    ByteReader(const u8* data, usize n) : p_(data), n_(n) {}

    bool get_u8(u8* v);
    bool get_u32(u32* v);
    bool get_u64(u64* v);
    bool get_bytes(void* out, usize n);
    bool get_str(Str* s); // view into the underlying buffer

    bool ok() const { return ok_; }
    usize pos() const { return pos_; }
    usize remaining() const { return n_ - pos_; }

  private:
    bool take(usize n);

    const u8* p_;
    usize n_;
    usize pos_ = 0;
    bool ok_ = true;
};

bool read_file(const char* path, Array<u8>* out);
bool write_file(const char* path, const void* data, usize n);

// Append-only text output (binary mode, so "\n" stays LF on Windows).
class TextFile {
  public:
    TextFile() = default;
    ~TextFile() { close(); }
    TextFile(const TextFile&) = delete;
    TextFile& operator=(const TextFile&) = delete;

    bool open(const char* path);
    bool is_open() const { return f_ != nullptr; }
    void write(Str s);
    void flush();
    void close();

  private:
    FILE* f_ = nullptr;
};

} // namespace nova
