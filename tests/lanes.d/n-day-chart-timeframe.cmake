if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_n_day_chart_timeframe)
    pineforge_add_unit_test(test_n_day_chart_timeframe)
endif()
