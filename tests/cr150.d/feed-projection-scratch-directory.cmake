if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_feed_projection_scratch_directory)
    pineforge_add_unit_test(test_feed_projection_scratch_directory)
endif()
