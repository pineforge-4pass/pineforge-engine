# --- CR-150 core-cr150-m1 begin ---
# Static-read lane: compilation and execution belong to the supervisor's lab.
if(UNIX AND PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_command_scratch_extent)
    pineforge_add_unit_test(test_command_scratch_extent)
    set_tests_properties(test_command_scratch_extent PROPERTIES LABELS "slow" RUN_SERIAL TRUE)
endif()
# --- CR-150 core-cr150-m1 end ---
