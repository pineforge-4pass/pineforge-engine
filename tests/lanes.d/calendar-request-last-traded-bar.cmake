if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_calendar_request_last_traded_bar)
    pineforge_add_unit_test(test_calendar_request_last_traded_bar)
endif()
