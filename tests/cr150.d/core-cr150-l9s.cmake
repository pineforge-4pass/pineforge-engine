# CR-150 L9S: Python controls run in preflight and CTest; native proof uses existing lab targets.
add_test(NAME test_run_json_selected_sink_l9s COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink.py -k "not native")
# The L6B1 target is declared after this include loop; match its build guard.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
add_test(NAME test_run_json_selected_sink_native_l9s COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_L9S_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>" "PINEFORGE_L9S_PLANNER_SO=$<TARGET_FILE:pineforge_window_plan>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink.py -k native)
add_test(NAME test_run_json_selected_sink_memory_l9s COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_L9S_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>" "PINEFORGE_L9S_PLANNER_SO=$<TARGET_FILE:pineforge_window_plan>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink_memory.py)
set_tests_properties(test_run_json_selected_sink_memory_l9s PROPERTIES LABELS "slow;mem" TIMEOUT 2400)
endif()
