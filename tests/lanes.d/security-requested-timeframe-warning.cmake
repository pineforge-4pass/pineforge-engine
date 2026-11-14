if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_security_requested_timeframe_warning)
    pineforge_add_unit_test(test_security_requested_timeframe_warning)
endif()
