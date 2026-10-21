# CR-224: one real-native pytest row, after the ALL build; no duration limits.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
add_test(NAME test_run_json_jailed_nofile_e2e COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR224_STRATEGY_SO=$<TARGET_FILE:cr224_nofile_strategy>" ${Python3_EXECUTABLE} -B -m pytest -q -s -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_jailed_nofile_e2e.py)
endif()
