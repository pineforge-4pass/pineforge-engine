add_test(NAME test_issue_932_csv_timestamps_stdlib
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_issue_932_csv_timestamps.py" stdlib)
if(_pf_numpy_import STREQUAL "0")
    add_test(NAME test_issue_932_csv_timestamps_numpy
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_issue_932_csv_timestamps.py" numpy)
endif()
