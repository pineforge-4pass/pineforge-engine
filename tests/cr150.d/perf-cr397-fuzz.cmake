# Picked up by the existing sorted cr150.d include loop. The target expression
# resolves after the later perf-cr397-c2 fence defines the real strategy module.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_native_sink_fuzz COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>" ${Python3_EXECUTABLE} -B -m pytest -q -x -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_fuzz.py)
endif()
