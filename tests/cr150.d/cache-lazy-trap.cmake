if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_cache_lazy_trap)
    pineforge_add_unit_test(test_cache_lazy_trap)
endif()
