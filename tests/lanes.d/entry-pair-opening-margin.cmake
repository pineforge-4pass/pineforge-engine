if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_entry_pair_opening_margin)
    pineforge_add_unit_test(test_entry_pair_opening_margin)
endif()
