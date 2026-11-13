# Command scratch arena exhaustion rows, first set (the second set is in the matching fence).
if(UNIX)
    list(APPEND TEST_SOURCES test_command_scratch_arena_exhaustion)
    pineforge_add_unit_test(test_command_scratch_arena_exhaustion)
endif()
