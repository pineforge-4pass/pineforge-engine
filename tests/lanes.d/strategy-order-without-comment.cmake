if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_strategy_order_without_comment)
    pineforge_add_unit_test(test_strategy_order_without_comment)
endif()
