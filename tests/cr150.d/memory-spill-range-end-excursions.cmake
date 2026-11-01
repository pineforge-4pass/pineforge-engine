# Use the kernel host and the actual terminal excursion implementation.
if(UNIX)
    list(APPEND TEST_SOURCES test_memory_spill_range_end_excursions)
    pineforge_add_unit_test(test_memory_spill_range_end_excursions)
endif()
