if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_exit_scratch_corrupt_birth)
    pineforge_add_unit_test(test_exit_scratch_corrupt_birth)
endif()
