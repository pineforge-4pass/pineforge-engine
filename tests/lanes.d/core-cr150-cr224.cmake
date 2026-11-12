# CR-281 / CR-224: three real-native assertions after the ALL build; no clocks.
if(PINEFORGE_BUILD_SOURCE_LAYER AND EXISTS "${PROJECT_SOURCE_DIR}/corpus/validation/bracket-same-id-exit-replace-01/generated.cpp" AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
add_test(NAME test_run_json_jailed_nofile_e2e COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR224_STRATEGY_SO=$<TARGET_FILE:cr224_nofile_strategy>" ${Python3_EXECUTABLE} -B -m pytest -vv -s -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_jailed_nofile_e2e.py)
endif()
