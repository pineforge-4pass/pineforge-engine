# Native-only witnesses: register in source and kernel profiles alike.
list(APPEND TEST_SOURCES test_memory_command_keys)
pineforge_add_unit_test(test_memory_command_keys)
