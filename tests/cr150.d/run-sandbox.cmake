if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_run_sandbox)
    pineforge_add_unit_test(test_run_sandbox)
endif()
