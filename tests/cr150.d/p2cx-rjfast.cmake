# CR150-CX / S-272: piecewise byte oracle and report writer call bounds.
add_test(NAME test_run_json_stream_fastpath COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_stream_fastpath.py)
set_tests_properties(test_run_json_stream_fastpath PROPERTIES LABELS "fast")
