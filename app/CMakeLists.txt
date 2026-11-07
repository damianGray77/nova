# The `nova` command line: runs a config, writes stats / replay / snapshot
# outputs, prints the world hash; verifies a replay log; benchmarks the
# interpreter; or opens the analysis UI (nova view).

# The analysis UI's drawing half (Phase 6): the GDI canvas and the viewer's
# panels. A library so the tests can drive it headless. user32 and gdi32 are
# Windows' own platform libraries.
add_library(nova_viewer STATIC canvas.cpp viewer.cpp)
target_link_libraries(nova_viewer PUBLIC nova::core user32 gdi32)
target_include_directories(nova_viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(nova_viewer PRIVATE /W4 /permissive-)
endif()

add_executable(nova main.cpp bench.cpp bench_costs.cpp bench_warp.cpp view_cmd.cpp)
target_link_libraries(nova PRIVATE nova::core nova_viewer)
if(MSVC)
    target_compile_options(nova PRIVATE /W4 /permissive-)
endif()
