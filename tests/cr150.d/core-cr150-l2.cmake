# CR-150 L2: the preflight harness also discovers docker/run_json_bin_test.py.
add_test(NAME test_run_json_bin COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_bin.py)
