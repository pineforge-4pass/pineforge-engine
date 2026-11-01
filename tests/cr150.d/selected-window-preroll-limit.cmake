# The fake-based rows stay unconditional; the real CLI rows are registered in
# the owning source-layer, non-instrumented fence in tests/CMakeLists.txt.
add_test(NAME test_run_json_selected_window
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_window.py
        -k "not real_cli"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
