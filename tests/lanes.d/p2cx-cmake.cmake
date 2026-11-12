# P2 CMake: H2-6 proves skips fail through the real registered L9A driver.
add_test(NAME test_pytest_no_skips_controls COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_run_pytest_no_skips.py --artifacts ${CMAKE_CURRENT_BINARY_DIR}/pytest-no-skips-controls)

# H2-6: outputs_fixture_strategy already exists here. L6B1 is declared after
# the include loop under PINEFORGE_BUILD_SOURCE_LAYER AND UNIX, so use that
# same guard rather than testing its target before the declaration. Both
# TARGET_FILE operands must resolve at generation; this row has no fallback.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND TARGET outputs_fixture_strategy AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_sink_l9a_native COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_L9A_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>" "PINEFORGE_L9A_OUTPUTS_SO=$<TARGET_FILE:outputs_fixture_strategy>" ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/run_pytest_no_skips.py --artifacts ${CMAKE_CURRENT_BINARY_DIR}/pytest-no-skips/test_run_json_sink_l9a_native -- -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink.py -k "native and not corpus")
endif()
