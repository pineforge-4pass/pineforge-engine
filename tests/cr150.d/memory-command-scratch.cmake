# Check command-window containers while continuing after earlier failures.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_command_scratch)
    pineforge_add_unit_test(test_memory_command_scratch)
endif()
