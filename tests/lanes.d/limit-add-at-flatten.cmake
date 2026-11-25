if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_limit_add_at_flatten)
    pineforge_add_unit_test(test_limit_add_at_flatten)
endif()
