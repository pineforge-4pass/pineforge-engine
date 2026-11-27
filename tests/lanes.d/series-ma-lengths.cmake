if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_series_ma_lengths)
    pineforge_add_unit_test(test_series_ma_lengths)
endif()
