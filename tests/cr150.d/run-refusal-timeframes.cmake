if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_run_refusal_timeframes)
    pineforge_add_unit_test(test_run_refusal_timeframes)
endif()
