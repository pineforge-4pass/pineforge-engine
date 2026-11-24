if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_flat_margin)
    pineforge_add_unit_test(test_reversal_purge_flat_margin)
endif()
