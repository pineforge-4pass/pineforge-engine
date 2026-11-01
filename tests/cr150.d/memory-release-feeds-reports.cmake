if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_memory_release_feeds_reports)
    pineforge_add_unit_test(test_memory_release_feeds_reports)
endif()
