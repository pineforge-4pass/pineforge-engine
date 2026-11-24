if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pyramiding_quantity_step)
    pineforge_add_unit_test(test_pyramiding_quantity_step)
endif()
