if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_margin_call_long_tick_mark)
    pineforge_add_unit_test(test_margin_call_long_tick_mark)
endif()
