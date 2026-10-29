if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_engine_layout_probe)
    # The standalone target is defined in the matching CMake target block.
    add_test(NAME test_engine_layout_probe COMMAND test_engine_layout_probe)
endif()
