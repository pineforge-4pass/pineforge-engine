if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_o1_report_contract)
    pineforge_add_unit_test(test_o1_report_contract)
endif()
