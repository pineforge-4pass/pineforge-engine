if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_entry_amount_close_continuation)
    pineforge_add_unit_test(test_entry_amount_close_continuation)
endif()
