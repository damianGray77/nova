# The `nova` command line: runs a config, writes stats / replay / snapshot
# outputs, prints the world hash; verifies a replay log; or benchmarks the
# interpreter.
add_executable(nova main.cpp bench.cpp)
target_link_libraries(nova PRIVATE nova::core)
if(MSVC)
    target_compile_options(nova PRIVATE /W4 /permissive-)
endif()
