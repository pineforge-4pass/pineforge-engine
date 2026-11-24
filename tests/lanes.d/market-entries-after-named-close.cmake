if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_market_entries_after_named_close)
    pineforge_add_unit_test(test_market_entries_after_named_close)
endif()
