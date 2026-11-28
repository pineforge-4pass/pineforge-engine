if(TARGET test_native_instrument_feed_twin)
    add_test(NAME test_native_instrument_feed_twin_failed_run
        COMMAND ${CMAKE_COMMAND} -DTWIN=$<TARGET_FILE:test_native_instrument_feed_twin>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_instrument_feed_twin.cmake)
endif()
