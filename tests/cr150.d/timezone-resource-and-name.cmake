if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_timezone_resource_and_name)
    pineforge_add_unit_test(test_timezone_resource_and_name)
endif()
