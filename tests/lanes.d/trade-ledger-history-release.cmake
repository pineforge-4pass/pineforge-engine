if(UNIX)
    list(APPEND TEST_SOURCES test_trade_ledger_history_release)
    pineforge_add_unit_test(test_trade_ledger_history_release)
endif()
