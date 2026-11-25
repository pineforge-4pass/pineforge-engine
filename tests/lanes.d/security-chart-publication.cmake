if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_security_chart_publication)
    pineforge_add_unit_test(test_security_chart_publication)
endif()
