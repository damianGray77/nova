#pragma once

// Backed<T> (Phase 7b, design.md 6.13; plan C8): an array that lives in RAM,
// or -- once bound to a file -- in that file, memory-mapped, so Windows pages
// it in and out and it costs disk instead of RAM. The disk tier's per-codon
// arrays use it: with disk.path set they live on real disk, and because only
// the storage changes, a world gives exactly the same hash either way.
//
// The interface is the slice of Array the disk tier uses: size, data,
// indexing, resize (new elements zero), fill, assign, clear. A bound file is
// created fresh and deleted when it is closed (or the process ends): it is
// working storage, not a record -- snapshots carry the disk tier's contents.
#include <cstring> // memcpy

#include "nova/array.h"
#include "nova/types.h"

namespace nova {

// A file mapped whole into memory (the Win32 handles stay in mapped.cpp).
class FileMap {
  public:
    FileMap() = default;
    ~FileMap() { close(); }
    FileMap(const FileMap&) = delete;
    FileMap& operator=(const FileMap&) = delete;

    // Creates (or empties) the file; it is deleted when closed.
    bool open(const char* path);
    // Sizes the file to `bytes` and maps all of it (contents kept; growth is
    // zero). Returns the view, or nullptr (also for 0 bytes).
    void* map(u64 bytes);
    void close();
    bool is_open() const { return file_ != nullptr; }

  private:
    void unmap();
    void* file_ = nullptr;
    void* mapping_ = nullptr;
    void* view_ = nullptr;
    u64 bytes_ = 0;
};

// Folder helpers for the disk tier's files.
bool make_folder(const char* path); // true if it exists afterwards
void remove_folder(const char* path); // if empty
u32 process_id();

template <class T> class Backed {
    static_assert(__is_trivially_copyable(T), "nova::Backed holds trivially copyable types only");

  public:
    Backed() = default;
    Backed(const Backed&) = delete;
    Backed& operator=(const Backed&) = delete;

    // Moves the contents into a file at `path` (created; deleted when closed).
    bool bind(const char* path) {
        Array<T> keep;
        keep.assign(p_, n_);
        if (!map_.open(path))
            return false;
        file_ = true;
        const usize n = n_;
        n_ = 0;
        p_ = nullptr;
        resize(n);
        if (n)
            memcpy(static_cast<void*>(p_), keep.data(), n * sizeof(T));
        ram_ = Array<T>();
        return true;
    }
    bool in_file() const { return file_; }

    usize size() const { return n_; }
    bool empty() const { return n_ == 0; }
    T* data() { return p_; }
    const T* data() const { return p_; }
    T& operator[](usize i) {
        NOVA_ASSERT(i < n_);
        return p_[i];
    }
    const T& operator[](usize i) const {
        NOVA_ASSERT(i < n_);
        return p_[i];
    }

    void resize(usize n) {
        if (!file_) {
            ram_.resize(n);
            p_ = ram_.data();
            n_ = n;
            return;
        }
        if (n == n_)
            return;
        // Resizing the file keeps what's there and zero-fills growth; a
        // shrink drops the tail, so a later growth reads zeros, as Array does.
        p_ = static_cast<T*>(map_.map(static_cast<u64>(n) * sizeof(T)));
        if (n && !p_)
            NOVA_FATAL("Backed: cannot map the disk tier's file (disk full?)");
        n_ = n;
    }
    void clear() { resize(0); }
    void fill(const T& v) {
        for (usize i = 0; i < n_; ++i)
            p_[i] = v;
    }
    void assign(const T* src, usize n) {
        resize(n);
        if (n)
            memcpy(static_cast<void*>(p_), src, n * sizeof(T));
    }

  private:
    Array<T> ram_;
    FileMap map_;
    T* p_ = nullptr;
    usize n_ = 0;
    bool file_ = false;
};

} // namespace nova
