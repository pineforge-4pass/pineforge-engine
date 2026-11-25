if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_waiting_void_global_exit)
    pineforge_add_unit_test(test_waiting_void_global_exit)
endif()
