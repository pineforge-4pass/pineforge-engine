if(UNIX)
    list(APPEND TEST_SOURCES test_allocation_census)
    pineforge_add_unit_test(test_allocation_census)
endif()
