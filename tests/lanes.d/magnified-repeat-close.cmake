if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_magnified_repeat_close)
    pineforge_add_unit_test(test_magnified_repeat_close)
endif()
