if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_cache_sweep_id_pages)
    pineforge_add_unit_test(test_cache_sweep_id_pages)
endif()
