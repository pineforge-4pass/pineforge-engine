if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_entry_behind_unfilled_close)
    pineforge_add_unit_test(test_entry_behind_unfilled_close)
endif()
