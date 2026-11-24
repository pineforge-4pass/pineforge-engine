if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_map_scratch_buffered_io)
    pineforge_add_unit_test(test_map_scratch_buffered_io)
endif()
