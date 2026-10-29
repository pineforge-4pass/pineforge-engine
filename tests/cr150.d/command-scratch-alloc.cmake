if(UNIX AND PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_command_scratch_alloc_failure)
    pineforge_add_unit_test(test_command_scratch_alloc_failure)
endif()
