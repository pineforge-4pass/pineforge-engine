if(UNIX)
    list(APPEND TEST_SOURCES test_report_sink_summary)
    pineforge_add_unit_test(test_report_sink_summary)
endif()
