if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_placement_projection)
    pineforge_add_unit_test(test_memory_placement_projection)
endif()
