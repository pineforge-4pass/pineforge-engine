if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue978_exit_scratch_activation)
    pineforge_add_unit_test(test_issue978_exit_scratch_activation)
endif()
