# F1 / round 3: real-sink trade scan, red first on 2561cfb464.
# Preflight also discovers docker/run_json_*_test.py (the L9A convention).
add_test(NAME test_run_json_trade_scan COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_trade_scan_test.py)
set_tests_properties(test_run_json_trade_scan PROPERTIES LABELS "fast")
