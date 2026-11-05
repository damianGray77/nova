cmake_minimum_required(VERSION 3.21)

project(nova
    VERSION 0.1.0
    DESCRIPTION "Nova: a Tierra-inspired digital ecosystem (see docs/design.md)"
    LANGUAGES CXX)

# --- global language settings ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Predictable output location so the verify loop can find the binaries.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# core/ is the dependency-free engine: no STL, no third-party code (design.md 7.1 B).
# app/ is the thin `nova` command line around it.
add_subdirectory(core)
add_subdirectory(app)

option(NOVA_BUILD_TESTS "Build the Nova unit tests" ON)
if(NOVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
