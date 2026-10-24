if(UNIX)
    list(APPEND TEST_SOURCES test_report_mode_v1_summary_with_observer)
    pineforge_add_unit_test(test_report_mode_v1_summary_with_observer)
endif()
