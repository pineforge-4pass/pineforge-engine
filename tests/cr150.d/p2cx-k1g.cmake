if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_k1_dormant_reissue_growth)
    pineforge_add_unit_test(test_k1_dormant_reissue_growth)
    # Two lengths, untouched and two armed-touch rates, each ERASE vs RETAIN.
    # Successful stdout is the measurement; use CTest's verbose output.
    set_tests_properties(test_k1_dormant_reissue_growth PROPERTIES LABELS "slow;mem")
endif()
