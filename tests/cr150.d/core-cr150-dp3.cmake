# CR-150 CX-D-P3: successor-root scratch backend and run-directory binding.
if(UNIX)
    list(APPEND TEST_SOURCES test_successor_roots_arena)
    pineforge_add_unit_test(test_successor_roots_arena)
    set_tests_properties(test_successor_roots_arena PROPERTIES TIMEOUT 600)
endif()
