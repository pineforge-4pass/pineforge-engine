if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_tick_exit_fill_recalculation)
    pineforge_add_unit_test(test_tick_exit_fill_recalculation)
endif()
