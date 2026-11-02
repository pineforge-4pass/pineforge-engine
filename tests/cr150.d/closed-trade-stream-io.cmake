if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_closed_trade_stream_io)
    pineforge_add_unit_test(test_closed_trade_stream_io)
endif()
