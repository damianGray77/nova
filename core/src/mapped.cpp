#include "nova/mapped.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace nova {

bool FileMap::open(const char* path) {
    close();
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    file_ = f;
    bytes_ = 0;
    return true;
}

void FileMap::unmap() {
    if (view_)
        UnmapViewOfFile(view_);
    if (mapping_)
        CloseHandle(static_cast<HANDLE>(mapping_));
    view_ = mapping_ = nullptr;
}

void* FileMap::map(u64 bytes) {
    if (!file_)
        return nullptr;
    unmap(); // a mapped file can't change size
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(static_cast<HANDLE>(file_), at, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(static_cast<HANDLE>(file_)))
        return nullptr;
    bytes_ = bytes;
    if (bytes == 0)
        return nullptr;
    HANDLE m = CreateFileMappingA(static_cast<HANDLE>(file_), nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!m)
        return nullptr;
    mapping_ = m;
    view_ = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(bytes));
    return view_;
}

void FileMap::close() {
    unmap();
    if (file_)
        CloseHandle(static_cast<HANDLE>(file_)); // deletes it (FILE_FLAG_DELETE_ON_CLOSE)
    file_ = nullptr;
    bytes_ = 0;
}

bool make_folder(const char* path) {
    if (CreateDirectoryA(path, nullptr))
        return true;
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void remove_folder(const char* path) { RemoveDirectoryA(path); }

u32 process_id() { return static_cast<u32>(GetCurrentProcessId()); }

} // namespace nova
