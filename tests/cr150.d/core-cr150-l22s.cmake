if(UNIX)
    list(APPEND TEST_SOURCES test_scratch_arena)
    pineforge_add_unit_test(test_scratch_arena)
    # Needs a disk-backed scratch filesystem; fails on the CLOUD overlayfs container ("create backing descriptor").
    set_tests_properties(test_scratch_arena PROPERTIES LABELS "LAB-DISKSCRATCH")
    list(APPEND TEST_SOURCES test_scratch_arena_threads)
    pineforge_add_unit_test(test_scratch_arena_threads)
endif()
