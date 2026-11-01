if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_fixed_population)
    pineforge_add_unit_test(test_memory_fixed_population)
endif()
