if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_ledger_retired_cache_bound)
    pineforge_add_unit_test(test_ledger_retired_cache_bound)
endif()
