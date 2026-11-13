# Python controls run in preflight and CTest; native proof uses existing targets.
add_test(NAME test_run_json_selected_sink_l9s COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_sink.py -k "not native")
