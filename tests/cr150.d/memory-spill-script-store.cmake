# Check script-state records, retirement, retained extent and the object limit.
list(APPEND TEST_SOURCES test_memory_spill_script_store)
pineforge_add_unit_test(test_memory_spill_script_store)
