if(UNIX)
    list(APPEND TEST_SOURCES test_p2cx_ledger_budget_handle_reuse)
    pineforge_add_unit_test(test_p2cx_ledger_budget_handle_reuse)
endif()
