if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_coof_reused_id_reentry)
    pineforge_add_unit_test(test_coof_reused_id_reentry)
endif()
