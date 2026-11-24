if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_936)
    pineforge_add_unit_test(test_issue_936)
endif()
