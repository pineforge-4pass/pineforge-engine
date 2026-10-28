if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_scratch_mapped_read)
    pineforge_add_unit_test(test_scratch_mapped_read)
endif()
