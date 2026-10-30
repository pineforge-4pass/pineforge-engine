if(UNIX)
    list(APPEND TEST_SOURCES test_script_store_arena_exhaustion)
    pineforge_add_unit_test(test_script_store_arena_exhaustion)
endif()
