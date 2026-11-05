add_library(nova_core STATIC
    src/fatal.cpp
    src/str.cpp
    src/io.cpp
    src/config.cpp
    src/pool.cpp
    src/world.cpp
    src/snapshot.cpp
    src/stats.cpp
    src/replay.cpp
    src/run.cpp
    src/genebank.cpp
    src/compress.cpp
    src/isa.cpp
    src/soup.cpp
    src/interp.cpp
)
add_library(nova::core ALIAS nova_core)

target_include_directories(nova_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(nova_core PUBLIC cxx_std_20)

if(MSVC)
    # fopen/strtod etc. are used deliberately (thin C runtime, design.md 7.1 B);
    # silence MSVC's "use the _s variant" deprecation noise rather than tie the
    # code to Annex K.
    target_compile_definitions(nova_core PUBLIC _CRT_SECURE_NO_WARNINGS)
    # Keep core warning-clean from day one. Warnings-as-errors is left to CI so a
    # stray warning never blocks the fast local loop.
    target_compile_options(nova_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(nova_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
