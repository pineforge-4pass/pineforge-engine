if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_active_order_ids_sized_from_run)
    pineforge_add_unit_test(test_active_order_ids_sized_from_run)
endif()
