if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_split_session_half_day_request)
    pineforge_add_unit_test(test_split_session_half_day_request)
endif()
