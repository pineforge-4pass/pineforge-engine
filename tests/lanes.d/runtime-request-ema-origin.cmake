if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_runtime_request_ema_origin)
    pineforge_add_unit_test(test_runtime_request_ema_origin)
endif()
