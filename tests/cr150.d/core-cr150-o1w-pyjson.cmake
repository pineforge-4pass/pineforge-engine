# R19–R22: fixed writer formatting and Python transactional report transport.
if(UNIX)
    list(APPEND TEST_SOURCES test_report_sink_writer_memory)
    pineforge_add_unit_test(test_report_sink_writer_memory)
    set_tests_properties(test_report_sink_writer_memory PROPERTIES LABELS "mem")
    add_test(NAME test_run_json_bounded_transport COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_bounded_transport_test.py)
    set_tests_properties(test_run_json_bounded_transport PROPERTIES LABELS "mem")
endif()
