if(UNIX AND PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_922_strategy_override)
    pineforge_add_unit_test(test_issue_922_strategy_override)
endif()
