list(APPEND TEST_SOURCES test_issue809_window_snapshot)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    pineforge_add_unit_test(test_issue809_window_snapshot)
endif()
