if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_exit_qty_entry_bar)
    pineforge_add_unit_test(test_exit_qty_entry_bar)
endif()
