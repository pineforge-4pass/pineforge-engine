if(UNIX)
    list(APPEND TEST_SOURCES test_security_trace_gaps)
    pineforge_add_unit_test(test_security_trace_gaps)
endif()
