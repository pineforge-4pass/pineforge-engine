if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue694_pyramid_tick_exits)
    pineforge_add_unit_test(test_issue694_pyramid_tick_exits)
endif()
