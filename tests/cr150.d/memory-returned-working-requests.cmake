# working_request_snapshot: public native host reads; also available without the source layer.
if(UNIX)
    list(APPEND TEST_SOURCES test_memory_returned_working_requests)
    pineforge_add_unit_test(test_memory_returned_working_requests)
endif()
