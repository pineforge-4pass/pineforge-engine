# Pine ta.mom / ta.roc source adapters: `source[length]` counted by position.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pine_mom_missing_bar)
    pineforge_add_unit_test(test_pine_mom_missing_bar)
    list(APPEND TEST_SOURCES test_pine_roc_missing_bar)
    pineforge_add_unit_test(test_pine_roc_missing_bar)
endif()
