if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_waiting_global_exit)
    pineforge_add_unit_test(test_reversal_purge_waiting_global_exit)
endif()
