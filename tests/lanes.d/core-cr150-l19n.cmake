# CR-150 L19N: the kernel calendar memo and the consumer's certified-cycle ring own a fixed heap.
list(APPEND TEST_SOURCES test_native_calendar_memo_bound)
pineforge_add_unit_test(test_native_calendar_memo_bound)
list(APPEND TEST_SOURCES test_native_certified_cycle_bound)
pineforge_add_unit_test(test_native_certified_cycle_bound)
