# CR-150 L9B: preflight discovers the same Python rows through docker's glob.
add_test(NAME test_run_json_sink_summary_l9b COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink_summary.py -k "not native")
# The native rows need the L6B1 and outputs module paths in their environment,
# which a lane file may not set. They are registered, with that environment, in
# the core-cr150-l9b fence at the end of tests/CMakeLists.txt.
