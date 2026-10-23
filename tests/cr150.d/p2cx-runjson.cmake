if(UNIX)
    add_test(NAME test_p2cx_run_json COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_p2cx_test.py)
endif()
