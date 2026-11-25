if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_market_reversal_at_entry_cap)
    pineforge_add_unit_test(test_market_reversal_at_entry_cap)
endif()
