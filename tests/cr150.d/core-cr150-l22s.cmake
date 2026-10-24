if(UNIX)
    list(APPEND TEST_SOURCES test_scratch_arena)
    pineforge_add_unit_test(test_scratch_arena)
    # Needs a disk-backed scratch filesystem; fails on the CLOUD overlayfs container ("create backing descriptor"); passes on the disk-backed NTU host (lab remote job rj-20261024t043230-9d8220).
    set_tests_properties(test_scratch_arena PROPERTIES LABELS "LAB-DISKSCRATCH")
    list(APPEND TEST_SOURCES test_scratch_arena_threads)
    pineforge_add_unit_test(test_scratch_arena_threads)
endif()
