if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_priced_percent_reversal)
    pineforge_add_unit_test(test_priced_percent_reversal)
endif()
