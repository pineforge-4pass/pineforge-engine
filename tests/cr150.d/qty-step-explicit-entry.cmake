if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_qty_step_explicit_entry)
    pineforge_add_unit_test(test_qty_step_explicit_entry)
endif()
