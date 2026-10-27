# CR-397 option (c): source characterization uses the strict-only discovery shim.
# The lane fence registers the same matrix with both routes and a real module.
# Preflight also discovers docker/run_json_*_test.py (no build needed).
# No "fast" label: PR Release runs ctest -L '^fast$' without pytest, like the
# other pytest rows in tests/CMakeLists.txt.
add_test(NAME test_run_json_sink_refusal_matrix COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_sink_refusal_matrix_test.py)
