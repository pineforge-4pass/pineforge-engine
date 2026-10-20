# CR-150 L9B: preflight discovers the same Python rows through docker's glob.
add_test(NAME test_run_json_sink_summary_l9b COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_summary.py -k "not native")
# L6B1 creates its module after the lane include loop. Match its source/UNIX
# conditions here; TARGET_FILE resolves after that later declaration.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_sink_summary_l9b_native COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_L9B_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>" "PINEFORGE_L9B_OUTPUTS_SO=$<TARGET_FILE:outputs_fixture_strategy>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_summary.py -k native)
endif()
