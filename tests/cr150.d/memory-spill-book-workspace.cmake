# Observe the run-owned dormant-book workspace through a native host.
list(APPEND TEST_SOURCES test_memory_spill_book_workspace)
pineforge_add_unit_test(test_memory_spill_book_workspace)
# The run executes 337,527 checks and takes 20 to 35 minutes in a Release build, depending on the machine.
set_tests_properties(test_memory_spill_book_workspace PROPERTIES LABELS slow)
