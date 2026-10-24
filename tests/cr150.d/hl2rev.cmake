if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_hl2rev_reversal_io)
    pineforge_add_unit_test(test_hl2rev_reversal_io)
endif()
