# CSV loader byte/hash equivalence, refusals, batching and working-memory bounds.
add_test(NAME test_run_json_csv_bars COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_csv_bars.py)
