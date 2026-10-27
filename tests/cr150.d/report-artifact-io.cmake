if(UNIX)
    add_test(NAME test_run_json_artifact_io COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_artifact_io.py)
endif()
