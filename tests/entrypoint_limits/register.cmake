add_test(NAME test_entrypoint_compile_limits
    COMMAND ${CMAKE_COMMAND} -E env
        "PINEFORGE_ENTRYPOINT_TEST_ARCHIVE=$<TARGET_FILE:pineforge>"
        ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/entrypoint_limits/test_compile_limits.py)
set_tests_properties(test_entrypoint_compile_limits PROPERTIES TIMEOUT 300)
