# A public, stdlib-only script suite, like test_ci_preflight.
add_test(NAME test_emit_abi_export_list
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_emit_abi_export_list.py)
