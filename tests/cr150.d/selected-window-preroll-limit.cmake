# The fake-based rows stay unconditional; the real CLI rows are registered in
# the owning non-instrumented fence.
add_test(NAME test_run_json_selected_window
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_selected_window.py
        -k "not real_cli"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
