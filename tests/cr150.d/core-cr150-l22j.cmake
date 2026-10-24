if(UNIX)
    add_test(NAME test_sink_to_json COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/pineforge_sink_to_json_test.py)
endif()
