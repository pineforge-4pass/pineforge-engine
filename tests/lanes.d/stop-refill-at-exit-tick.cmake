if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_stop_refill_at_exit_tick)
    pineforge_add_unit_test(test_stop_refill_at_exit_tick)
endif()
