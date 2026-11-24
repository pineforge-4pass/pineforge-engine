if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue694_security_site_projection)
    pineforge_add_unit_test(test_issue694_security_site_projection)
endif()
