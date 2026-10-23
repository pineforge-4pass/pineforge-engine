if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_ledger_history_scale)
    pineforge_add_unit_test(test_ledger_history_scale)
    set_tests_properties(test_ledger_history_scale PROPERTIES LABELS "slow;mem")
    list(APPEND TEST_SOURCES test_ledger_history_scale_400k)
    pineforge_add_unit_test(test_ledger_history_scale_400k)
    set_tests_properties(test_ledger_history_scale_400k PROPERTIES LABELS "slow;mem")
    # One size that runs in every profile, the Debug-type ones included (they exclude
    # `slow`), so the Debug-only closed-row check sees a ledger that spills its window.
    list(APPEND TEST_SOURCES test_ledger_history_scale_4k)
    pineforge_add_unit_test(test_ledger_history_scale_4k)
    set_tests_properties(test_ledger_history_scale_4k PROPERTIES LABELS "mem")
endif()
