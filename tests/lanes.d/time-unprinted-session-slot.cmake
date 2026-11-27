# time() / time_close() reading a later bar over a session slot the feed holds no bar for.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_time_unprinted_session_slot)
    pineforge_add_unit_test(test_time_unprinted_session_slot)
endif()
