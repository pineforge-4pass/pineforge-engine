if(UNIX)
    list(APPEND TEST_SOURCES test_report_sink_value_too_large)
    pineforge_add_unit_test(test_report_sink_value_too_large)
endif()
