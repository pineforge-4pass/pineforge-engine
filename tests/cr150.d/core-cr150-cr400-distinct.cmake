if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_cr400_distinct_ids_admitted)
    pineforge_add_unit_test(test_cr400_distinct_ids_admitted)
endif()
