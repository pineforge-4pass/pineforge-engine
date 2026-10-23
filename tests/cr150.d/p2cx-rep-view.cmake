if(UNIX)
    list(APPEND TEST_SOURCES test_selected_report_validation_only)
    pineforge_add_unit_test(test_selected_report_validation_only)
endif()
