if(UNIX)
    list(APPEND TEST_SOURCES test_sink_stall_budget)
    pineforge_add_unit_test(test_sink_stall_budget)
    list(APPEND TEST_SOURCES test_run_budget)
    pineforge_add_unit_test(test_run_budget)
    add_test(NAME test_run_json_budgets
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_run_json_budgets.py)
endif()
