if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_empty_request_timeframe)
    pineforge_add_unit_test(test_empty_request_timeframe)
endif()
