if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_versions_codec_external_openings)
    pineforge_add_unit_test(test_versions_codec_external_openings)
endif()
