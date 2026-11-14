if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_exit_refire_after_own_fill)
    pineforge_add_unit_test(test_exit_refire_after_own_fill)
endif()
