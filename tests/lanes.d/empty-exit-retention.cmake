if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue858_empty_exit_retention)
    pineforge_add_unit_test(test_issue858_empty_exit_retention)
endif()
