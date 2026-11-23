if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue979_exit_scratch_restore)
    pineforge_add_unit_test(test_issue979_exit_scratch_restore)
endif()
