if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_ledger_block_read)
    pineforge_add_unit_test(test_ledger_block_read)
endif()
