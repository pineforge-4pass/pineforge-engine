add_test(NAME test_entrypoint_cpp_attest COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_entrypoint_cpp_attest.py)
