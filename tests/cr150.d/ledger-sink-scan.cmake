if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_ledger_sink_scan)
    pineforge_add_unit_test(test_ledger_sink_scan)
endif()
