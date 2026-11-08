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
    test_genebank.cpp
    test_isa.cpp
    test_interp.cpp
    test_tierra.cpp
    test_soup.cpp
    test_heat.cpp
    test_contact.cpp
    test_rules.cpp
    test_tables.cpp
    test_detect.cpp
    test_gpu.cpp
    test_npu.cpp
    test_dispersal.cpp
    test_phase4.cpp
    test_phase5.cpp
    test_view.cpp
    test_setup.cpp
    test_landscape.cpp
    test_budget.cpp
    test_warp.cpp
    test_costs.cpp
    test_ecology.cpp
    test_keys.cpp
    test_substrates.cpp
    test_tiers.cpp
    test_disasters.cpp
    test_memtiers.cpp # memory tiers (docs/world-plan.md W3)
    # Learning from the runs (docs/analysis-plan.md).
    test_analysis.cpp
    # Behaviour profiles (docs/analysis-plan.md A3).
    test_profiles.cpp
    # The viewer and the live novelty meter (docs/results/viewer.md), in suites view and viewer.
    test_viewer_more.cpp
    # Gestation, ray hazards, wipe warnings and voluntary dormancy (docs/lifehistory-plan.md).
    test_lifehistory.cpp
)
target_link_libraries(nova_tests PRIVATE nova::core nova_viewer)
target_compile_features(nova_tests PRIVATE cxx_std_20)
# The keys suite loads the presets in configs/. The viewer suite runs nova.exe
# on a setup page's run.cfg (nova run and nova view must hash as the page's run), and the analysis
# suite's tiny search runs its trials with it.
target_compile_definitions(nova_tests PRIVATE NOVA_SOURCE_DIR="${PROJECT_SOURCE_DIR}" NOVA_EXE="$<TARGET_FILE:nova>")
add_dependencies(nova_tests nova)
if(MSVC)
    target_compile_options(nova_tests PRIVATE /W4 /permissive-)
endif()

# One CTest entry per suite; verify.ps1 runs them all via ctest. Tests that write
# files do so in the build tree (the CTest working directory).
set(NOVA_SUITES containers str rng config pool world gate snapshot stats replay genebank
    isa interp tierra soup heat contact rules tables detect gpu npu dispersal leak edges disk salvage signals lifecycle async devices
    view viewer landscape budget warp costs ecology keys substrates tiers disasters
    memtiers # memory tiers (W3)
    analysis
    profiles # behaviour profiles (A3)
    lifehistory) # docs/lifehistory-plan.md
foreach(suite IN LISTS NOVA_SUITES)
    add_test(NAME ${suite} COMMAND nova_tests ${suite})
    set_tests_properties(${suite} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()

# The no-STL guard (see tools/check_includes.cmake).
add_test(NAME includes
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/tools/check_includes.cmake)
