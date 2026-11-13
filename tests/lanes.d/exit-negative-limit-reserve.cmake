if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_exit_negative_limit_reserve)
    pineforge_add_unit_test(test_exit_negative_limit_reserve)
endif()
