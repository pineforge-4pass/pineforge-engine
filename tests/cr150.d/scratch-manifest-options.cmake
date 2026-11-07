if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_scratch_manifest_options)
    pineforge_add_unit_test(test_scratch_manifest_options)
endif()
