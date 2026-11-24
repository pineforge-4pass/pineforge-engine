if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_930_avg_trade_percent)
    pineforge_add_unit_test(test_issue_930_avg_trade_percent)
endif()
