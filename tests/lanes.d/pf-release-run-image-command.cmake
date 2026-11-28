add_test(NAME test_pf_release_run_image_command COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_pf_release_run.py)
