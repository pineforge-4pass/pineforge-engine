list(APPEND TEST_SOURCES test_run_usage)
pineforge_add_unit_test(test_run_usage)
add_test(NAME test_run_json_usage
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/scripts/test_run_json_usage.py)
