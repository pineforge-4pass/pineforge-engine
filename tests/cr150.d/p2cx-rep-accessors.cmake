if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_close_cause_sealed_rows_contract)
    pineforge_add_unit_test(test_close_cause_sealed_rows_contract)
endif()
