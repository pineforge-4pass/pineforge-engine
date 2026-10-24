# CR-150 RAM host: one substantive test per file; no duration limits.
list(APPEND TEST_SOURCES test_scratch_defaults)
pineforge_add_unit_test(test_scratch_defaults)
add_test(NAME test_run_json_jailed_profile COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_jailed_profile.py)
add_test(NAME test_scratch_tables_docs COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_scratch_tables_docs.py)
