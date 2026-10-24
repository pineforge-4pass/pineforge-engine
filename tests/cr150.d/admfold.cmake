if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admfold_bar_io)
    pineforge_add_unit_test(test_admfold_bar_io)
endif()
