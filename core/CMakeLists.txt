add_library(nova_core STATIC
    src/fatal.cpp
    src/str.cpp
    src/io.cpp
    src/config.cpp
    src/pool.cpp
    src/world.cpp
    src/graph.cpp
    src/snapshot.cpp
    src/stats.cpp
    src/replay.cpp
    src/run.cpp
    src/genebank.cpp
    src/compress.cpp
    src/isa.cpp
    src/soup.cpp
    src/interp.cpp
    src/lineage.cpp
    src/accel_dx.cpp
    src/accel_ref.cpp
    # The analysis UI's testable half (Phase 6): view frames, the PNG writer.
    src/view.cpp
    src/png.cpp
    # Phase 7: landscape worlds; budgets and the disk tier in files.
    src/landscape.cpp
    src/budget.cpp
    src/mapped.cpp
    # Phase 8: warps (the interpreter itself is warp_isa.inc, shared with the GPU).
    src/warp.cpp
    # Phase 9: measured costs.
    src/cost.cpp
)
add_library(nova::core ALIAS nova_core)

# Phase 8: the warp interpreter's one source, embedded for the GPU to compile
# as HLSL (accel_dx.cpp includes the generated warp_isa_bytes.inc).
set(NOVA_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
foreach(part isa layout)
    add_custom_command(
        OUTPUT ${NOVA_GEN}/warp_${part}_bytes.inc
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/warp_${part}.inc
                -DOUT=${NOVA_GEN}/warp_${part}_bytes.inc -DNAME=kWarp${part}Source
                -P ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_${part}.inc ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
        COMMENT "Embedding warp_${part}.inc for the GPU")
    target_sources(nova_core PRIVATE ${NOVA_GEN}/warp_${part}_bytes.inc)
endforeach()
target_include_directories(nova_core PRIVATE ${NOVA_GEN})

# The real GPU and NPU (Phase 5): Windows' Direct3D 12, DXCore and DirectML,
# import libraries from the Windows SDK.
target_link_libraries(nova_core PUBLIC d3d12 dxcore DirectML d3dcompiler)

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
