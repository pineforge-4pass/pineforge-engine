# PART B PREPARED ONLY: supervisor commits after L6 relays the decision.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_command_scratch_missing_tmpdir)
    pineforge_add_unit_test(test_command_scratch_missing_tmpdir)
    set_tests_properties(test_command_scratch_missing_tmpdir PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endif()
