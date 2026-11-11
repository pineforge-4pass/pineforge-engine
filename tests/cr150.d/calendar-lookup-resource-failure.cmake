if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_calendar_lookup_resource_failure)
    pineforge_add_unit_test(test_calendar_lookup_resource_failure)
endif()
