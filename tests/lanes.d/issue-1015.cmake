if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_1015_coof_stop_entry_tick_reach)
    pineforge_add_unit_test(test_issue_1015_coof_stop_entry_tick_reach)
endif()
