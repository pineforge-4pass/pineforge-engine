if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_completion_flags_on_demand)
    pineforge_add_unit_test(test_completion_flags_on_demand)
endif()
