if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_syminfo_mincontract_qty_step)
    pineforge_add_unit_test(test_syminfo_mincontract_qty_step)
endif()
