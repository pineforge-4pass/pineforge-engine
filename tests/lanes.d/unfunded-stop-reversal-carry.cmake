if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_unfunded_stop_reversal_carry)
    pineforge_add_unit_test(test_unfunded_stop_reversal_carry)
endif()
