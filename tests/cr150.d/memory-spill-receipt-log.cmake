# Check receipt insertions, the fixed default tail and file retirement.
list(APPEND TEST_SOURCES test_memory_spill_receipt_log)
pineforge_add_unit_test(test_memory_spill_receipt_log)
