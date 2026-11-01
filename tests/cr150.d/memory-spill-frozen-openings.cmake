# Check external-opening freezes, exact bytes and shared-file lifetime.
if(UNIX)
    list(APPEND TEST_SOURCES test_memory_spill_frozen_openings)
    pineforge_add_unit_test(test_memory_spill_frozen_openings)
endif()
