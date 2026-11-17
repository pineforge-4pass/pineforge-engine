if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_calendar_request_parse_once)
    pineforge_add_unit_test(test_calendar_request_parse_once)
endif()
