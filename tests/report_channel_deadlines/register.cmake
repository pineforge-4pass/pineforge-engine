# Included by the supervisor from tests/CMakeLists.txt when integrating this
# isolated slice. The real syscall interposer requires the Linux CLOUD host.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME test_report_channel_deadlines
        COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
            -s ${PROJECT_SOURCE_DIR}/tests/report_channel_deadlines
            -p test_deadlines.py -v)
    set_tests_properties(test_report_channel_deadlines PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        TIMEOUT 180
        LABELS "report-channel")
endif()
