if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_split_session_request_inputs)
    pineforge_add_unit_test(test_split_session_request_inputs)
endif()
