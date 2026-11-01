# Check successor-root backing growth, reset and explicit slab limits.
list(APPEND TEST_SOURCES test_memory_spill_successor_roots)
pineforge_add_unit_test(test_memory_spill_successor_roots)
