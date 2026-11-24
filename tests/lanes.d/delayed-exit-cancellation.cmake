if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_delayed_exit_cancellation)
    pineforge_add_unit_test(test_delayed_exit_cancellation)
endif()
