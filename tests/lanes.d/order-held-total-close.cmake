if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_order_held_total_close)
    pineforge_add_unit_test(test_order_held_total_close)
endif()
