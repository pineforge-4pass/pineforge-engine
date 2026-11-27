if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_series_osc_length)
    pineforge_add_unit_test(test_series_osc_length)
endif()
