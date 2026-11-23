if(UNIX)
    list(APPEND TEST_SOURCES test_issue_920_bool_input)
    pineforge_add_unit_test(test_issue_920_bool_input)
endif()
