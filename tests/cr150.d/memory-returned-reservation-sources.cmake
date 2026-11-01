if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_memory_returned_reservation_sources)
    pineforge_add_unit_test(test_memory_returned_reservation_sources)
endif()
