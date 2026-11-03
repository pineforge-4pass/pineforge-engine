if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_tick_grid_canonical_controls)
    pineforge_add_unit_test(test_tick_grid_canonical_controls)
endif()
