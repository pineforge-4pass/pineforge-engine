if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_k1_dormant_reissue_scan_work)
    pineforge_add_unit_test(test_k1_dormant_reissue_scan_work)
endif()
