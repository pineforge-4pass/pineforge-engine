if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_engine_layout_probe)
    pineforge_add_unit_test(test_engine_layout_probe)
endif()
