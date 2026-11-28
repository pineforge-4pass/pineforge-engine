if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_requested_ema_first_chart_bar)
    pineforge_add_unit_test(test_requested_ema_first_chart_bar)
endif()
