if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue_931_series_lookback)
    pineforge_add_unit_test(test_issue_931_series_lookback)
endif()
