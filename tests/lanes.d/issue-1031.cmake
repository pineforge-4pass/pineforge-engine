if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_1031_percent_stop_money_tie)
    pineforge_add_unit_test(test_issue_1031_percent_stop_money_tie)
endif()
