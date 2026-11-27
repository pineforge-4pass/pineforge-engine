if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_series_length_stats)
    pineforge_add_unit_test(test_series_length_stats)
endif()
