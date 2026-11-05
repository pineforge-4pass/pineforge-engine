if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND TEST_SOURCES test_runtime_history_spool_fallback)
    pineforge_add_unit_test(test_runtime_history_spool_fallback)
endif()
