if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pending_entry_amount)
    pineforge_add_unit_test(test_pending_entry_amount)
endif()
