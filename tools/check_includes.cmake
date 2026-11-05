# No-STL guard (design.md 7.1 B), run as the `includes` CTest entry:
#   cmake -DROOT=<repo> -P tools/check_includes.cmake
#
# Fails if any source under core/, app/ or tests/ includes a system header that
# is not on the allowlist below, or uses `std::` in code (comment text after //
# is ignored). Project headers ("nova/...", "check.h") are always fine.
#
# Allowlist: thin C-runtime headers and platform headers only. Adding to it is a
# dependency decision -- discuss with Damian first (memory 2195 / 1642).
set(allowed
    cstddef   # size_t
    cstdint   # fixed-width integers
    cstdio    # FILE, fopen, fprintf, snprintf
    cstdlib   # malloc/realloc/free, strtod, _Exit
    cstring   # memcpy, memset, memcmp, strlen
    cmath     # (reserved for heat curves etc.)
    windows.h # threads, SRWLOCK, CONDITION_VARIABLE
    intrin.h  # _umul128, _Interlocked*
)

if(NOT ROOT)
    message(FATAL_ERROR "check_includes: pass -DROOT=<repo root>")
endif()

file(GLOB_RECURSE sources
    "${ROOT}/core/*.h" "${ROOT}/core/*.cpp"
    "${ROOT}/app/*.h" "${ROOT}/app/*.cpp"
    "${ROOT}/tests/*.h" "${ROOT}/tests/*.cpp")

set(problems "")
foreach(f IN LISTS sources)
    file(STRINGS "${f}" include_lines REGEX "^[ \t]*#[ \t]*include[ \t]*<")
    foreach(line IN LISTS include_lines)
        string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>.*$" "\\1" hdr "${line}")
        if(NOT hdr IN_LIST allowed)
            list(APPEND problems "${f}: #include <${hdr}> is not on the allowlist")
        endif()
    endforeach()
    # `std::` in code: any occurrence with no `//` earlier on the line.
    file(STRINGS "${f}" std_lines REGEX "^([^/]|/[^/])*std::")
    foreach(line IN LISTS std_lines)
        list(APPEND problems "${f}: uses std:: -> ${line}")
    endforeach()
endforeach()

list(LENGTH sources n)
if(problems)
    foreach(p IN LISTS problems)
        message("  ${p}")
    endforeach()
    message(FATAL_ERROR "check_includes: no-STL rule broken (see above)")
endif()
message("check_includes: ${n} sources clean (no STL headers, no std::)")
