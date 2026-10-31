if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The test environment supplies pytest; a missing module fails this row.
    add_test(NAME test_quota_convert
        COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/docker/quota_convert_test.py)
endif()
