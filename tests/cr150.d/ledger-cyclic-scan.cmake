if(UNIX)
    list(APPEND TEST_SOURCES test_ledger_cyclic_scan)
    pineforge_add_unit_test(test_ledger_cyclic_scan)
endif()
