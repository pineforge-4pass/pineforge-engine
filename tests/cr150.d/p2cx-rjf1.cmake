# Test real-sink trade readback; preflight also discovers docker/run_json_*_test.py.
# No "fast" label: PR Release runs ctest -L '^fast$' without pytest, like the
# other pytest rows in tests/CMakeLists.txt.
add_test(NAME test_run_json_trade_scan COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_trade_scan_test.py)
