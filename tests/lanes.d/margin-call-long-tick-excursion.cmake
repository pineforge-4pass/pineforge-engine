if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_margin_call_long_tick_excursion)
    pineforge_add_unit_test(test_margin_call_long_tick_excursion)
endif()
