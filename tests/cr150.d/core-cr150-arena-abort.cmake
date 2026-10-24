if(UNIX)
    list(APPEND TEST_SOURCES test_report_scratch_arena_abort)
    pineforge_add_unit_test(test_report_scratch_arena_abort)
endif()
