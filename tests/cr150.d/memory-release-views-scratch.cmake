if(PINEFORGE_BUILD_SOURCE_LAYER)
    # This target owns a complete global counting allocator. Keep it separate.
    list(APPEND TEST_SOURCES test_memory_release_views_scratch)
    pineforge_add_unit_test(test_memory_release_views_scratch)
endif()
