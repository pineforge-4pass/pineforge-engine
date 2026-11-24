if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_applied_entry)
    pineforge_add_unit_test(test_reversal_purge_applied_entry)
endif()
