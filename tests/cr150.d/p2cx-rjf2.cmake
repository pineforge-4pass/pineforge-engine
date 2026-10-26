# F2 / round 2: real-sink equity scan, red first on F1 ff0a40ca0d89.
# Preflight also discovers docker/run_json_*_test.py (the L9A convention).
# No "fast" label: PR Release runs ctest -L '^fast$' without pytest, like the
# other pytest rows in tests/CMakeLists.txt.
add_test(NAME test_run_json_equity_scan COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_equity_scan_test.py)
