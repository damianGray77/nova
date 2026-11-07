add_library(nova_core STATIC
    src/fatal.cpp
    src/str.cpp
    src/io.cpp
    src/config.cpp
    # The settings registry (docs/config-plan.md C1): its table is src/keys.inc.
    src/keys.cpp
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
# The warp kernel itself (docs/dxc-plan.md D1): warp_isa.inc between a head
# and a main, embedded for FXC (any lane count, or warp.kernel = fxc).
add_custom_command(
    OUTPUT ${NOVA_GEN}/warp_kernel_bytes.inc
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/warp_kernel.hlsl
            -DOUT=${NOVA_GEN}/warp_kernel_bytes.inc -DNAME=kWarpkernelSource
            -P ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_kernel.hlsl ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
    COMMENT "Embedding warp_kernel.hlsl for the GPU")
target_sources(nova_core PRIVATE ${NOVA_GEN}/warp_kernel_bytes.inc)

# ... and compiled at build time by the Windows SDK's DXC (D2): for 32 lanes,
# the plain kernel and each groupshared tier, signed by the SDK's dxil.dll and
# embedded. A build tool only: no header and no DLL at run time. dxc.exe is
# looked for beside the SDK's rc.exe (the SDK the build already uses).
option(NOVA_DXC "Compile the warp kernel with the Windows SDK's DXC at build time" ON)
if(NOVA_DXC AND CMAKE_RC_COMPILER)
    get_filename_component(nova_sdk_bin "${CMAKE_RC_COMPILER}" DIRECTORY)
    find_program(NOVA_DXC_EXE NAMES dxc HINTS "${nova_sdk_bin}" NO_DEFAULT_PATH)
endif()
if(NOVA_DXC AND NOVA_DXC_EXE)
    message(STATUS "nova: the warp kernel is compiled by ${NOVA_DXC_EXE}")
    foreach(lds 0 256 512 1024 2048)
        set(dxil ${NOVA_GEN}/warp_32_${lds}.dxil)
        add_custom_command(
            OUTPUT ${dxil}
            COMMAND ${NOVA_DXC_EXE} -nologo -T cs_6_0 -E main -O3 -D W_LANES=32 -D W_LDS=${lds}
                    -I ${CMAKE_CURRENT_SOURCE_DIR}/src -Fo ${dxil} ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_kernel.hlsl
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_kernel.hlsl ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_isa.inc
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/warp_layout.inc
            COMMENT "Compiling the warp kernel with DXC (32 lanes, groupshared ${lds})")
        add_custom_command(
            OUTPUT ${NOVA_GEN}/warp_dxil_32_${lds}.inc
            COMMAND ${CMAKE_COMMAND} -DIN=${dxil} -DOUT=${NOVA_GEN}/warp_dxil_32_${lds}.inc
                    -DNAME=kWarpDxil32_${lds} -P ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
            DEPENDS ${dxil} ${PROJECT_SOURCE_DIR}/tools/embed_bytes.cmake
            COMMENT "Embedding the DXC warp kernel (groupshared ${lds})")
        target_sources(nova_core PRIVATE ${NOVA_GEN}/warp_dxil_32_${lds}.inc)
    endforeach()
    target_compile_definitions(nova_core PRIVATE NOVA_DXC=1)
else()
    message(STATUS "nova: no DXC (dxc.exe beside the SDK's rc.exe, or NOVA_DXC=OFF): "
                   "the warp kernel is compiled by FXC at run time")
endif()
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
