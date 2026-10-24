if(UNIX)
    list(APPEND TEST_SOURCES test_optional_report_sequences)
    pineforge_add_unit_test(test_optional_report_sequences)
endif()
