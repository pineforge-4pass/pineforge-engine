if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_void_setter_failure)
    pineforge_add_unit_test(test_void_setter_failure)
endif()

add_test(NAME test_run_json_syminfo_values
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_syminfo_values.py)
