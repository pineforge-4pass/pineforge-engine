if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_fill_executions_on_chart_legs)
    pineforge_add_unit_test(test_fill_executions_on_chart_legs)
endif()
