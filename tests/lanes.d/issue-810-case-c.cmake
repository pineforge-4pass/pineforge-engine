if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue810_case_c)
    pineforge_add_unit_test(test_issue810_case_c)
endif()
