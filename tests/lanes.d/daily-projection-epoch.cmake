if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_daily_projection_epoch)
    pineforge_add_unit_test(test_daily_projection_epoch)
endif()
