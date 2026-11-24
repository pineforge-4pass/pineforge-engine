if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_close_first_fill_qty)
    pineforge_add_unit_test(test_close_first_fill_qty)
endif()
