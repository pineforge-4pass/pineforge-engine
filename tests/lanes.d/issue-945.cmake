if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_adapter_external_openings_hash)
    pineforge_add_unit_test(test_adapter_external_openings_hash)
endif()
