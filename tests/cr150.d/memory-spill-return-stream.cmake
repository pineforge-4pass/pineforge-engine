# Inspect packed-return scratch storage through the real metrics implementation.
if(UNIX)
    list(APPEND TEST_SOURCES test_memory_spill_return_stream)
    pineforge_add_unit_test(test_memory_spill_return_stream)
endif()
