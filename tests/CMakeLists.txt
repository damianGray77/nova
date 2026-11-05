# Hand-rolled test executable: no third-party framework, no STL. test_main.cpp
# owns main(); with no argument it runs every suite, with a suite name it runs
# just that one (so each suite gets its own CTest entry below). Add a suite by
# dropping in test_<thing>.cpp, registering it in test_main.cpp, listing the
# source here, and adding it to NOVA_SUITES.
add_executable(nova_tests
    test_main.cpp
    test_containers.cpp
    test_str.cpp
    test_rng.cpp
    test_config.cpp
    test_pool.cpp
    test_world.cpp
    test_gate.cpp
    test_snapshot.cpp
    test_stats.cpp
    test_replay.cpp
    test_stubs.cpp
    test_isa.cpp
    test_interp.cpp
    test_tierra.cpp
    test_soup.cpp
    test_heat.cpp
    test_contact.cpp
)
target_link_libraries(nova_tests PRIVATE nova::core)
target_compile_features(nova_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(nova_tests PRIVATE /W4 /permissive-)
endif()

# One CTest entry per suite; verify.ps1 runs them all via ctest. Tests that write
# files do so in the build tree (the CTest working directory).
set(NOVA_SUITES containers str rng config pool world gate snapshot stats replay stubs
    isa interp tierra soup heat contact)
foreach(suite IN LISTS NOVA_SUITES)
    add_test(NAME ${suite} COMMAND nova_tests ${suite})
    set_tests_properties(${suite} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# The no-STL guard (see tools/check_includes.cmake).
add_test(NAME includes
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/tools/check_includes.cmake)
