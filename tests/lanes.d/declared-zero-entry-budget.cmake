if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_declared_zero_entry_budget)
    pineforge_add_unit_test(test_declared_zero_entry_budget)
endif()
