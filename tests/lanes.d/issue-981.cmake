if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_bind_openings_readers)
    pineforge_add_unit_test(test_bind_openings_readers)
endif()
