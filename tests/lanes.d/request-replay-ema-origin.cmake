if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_request_replay_ema_origin)
    pineforge_add_unit_test(test_request_replay_ema_origin)
endif()
