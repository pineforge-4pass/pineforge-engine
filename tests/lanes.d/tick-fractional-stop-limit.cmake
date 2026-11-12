if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_tick_fractional_stop_limit)
    pineforge_add_unit_test(test_tick_fractional_stop_limit)
endif()
