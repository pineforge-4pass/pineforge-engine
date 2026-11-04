if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_adapter_lookup_index_counts)
    pineforge_add_unit_test(test_adapter_lookup_index_counts)
endif()
