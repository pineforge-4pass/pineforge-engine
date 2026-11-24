if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_entry_replacement_close_claim)
    pineforge_add_unit_test(test_entry_replacement_close_claim)
endif()
