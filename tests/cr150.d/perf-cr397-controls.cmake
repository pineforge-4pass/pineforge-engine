# The shared include loop loads this before the generated module declaration.
# TARGET_FILE resolves at generation, after that existing target is declared.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_native_sink_controls
        COMMAND ${CMAKE_COMMAND} -E env
        "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_controls.py)
endif()
