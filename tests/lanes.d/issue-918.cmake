if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_918_input_rejection)
    pineforge_add_unit_test(test_issue_918_input_rejection)
endif()
