if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND TEST_SOURCES test_order_gauge)
    pineforge_add_unit_test(test_order_gauge)
endif()
