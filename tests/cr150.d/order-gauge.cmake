if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_order_gauge)
    pineforge_add_unit_test(test_order_gauge)
endif()
