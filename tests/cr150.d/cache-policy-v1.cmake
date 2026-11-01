if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_cache_policy_v1)
    pineforge_add_unit_test(test_cache_policy_v1)
endif()
