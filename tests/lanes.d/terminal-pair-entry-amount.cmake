if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_terminal_pair_entry_amount)
    pineforge_add_unit_test(test_terminal_pair_entry_amount)
endif()
