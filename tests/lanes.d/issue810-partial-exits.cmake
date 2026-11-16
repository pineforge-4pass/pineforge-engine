list(APPEND TEST_SOURCES test_issue810_partial_exits)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    pineforge_add_unit_test(test_issue810_partial_exits)
endif()
