if(UNIX)
    list(APPEND TEST_SOURCES test_run_sigterm_abort)
    pineforge_add_unit_test(test_run_sigterm_abort)
endif()
