# Real syscall/allocation interposition uses the Linux LP64 ABI.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND TEST_SOURCES test_command_scratch_spill)
    pineforge_add_unit_test(test_command_scratch_spill)
    set_tests_properties(test_command_scratch_spill PROPERTIES LABELS "scratch;cr397")
endif()
