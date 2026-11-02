if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The test environment supplies pytest; a missing module fails this row.
    add_test(NAME test_quota_columns
        COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/docker/quota_columns_test.py)
endif()
