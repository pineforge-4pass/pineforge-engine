# CR150 arena seam, set A. All execution belongs to the supervisor's lab.
if(UNIX)
    list(APPEND TEST_SOURCES test_command_scratch_arena_exhaustion)
    pineforge_add_unit_test(test_command_scratch_arena_exhaustion)
endif()
