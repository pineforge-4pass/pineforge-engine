if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_quota_ledger)
    pineforge_add_unit_test(test_quota_ledger)
endif()
