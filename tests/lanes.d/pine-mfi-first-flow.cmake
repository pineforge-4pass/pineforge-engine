if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pine_mfi_first_flow)
    pineforge_add_unit_test(test_pine_mfi_first_flow)
endif()
