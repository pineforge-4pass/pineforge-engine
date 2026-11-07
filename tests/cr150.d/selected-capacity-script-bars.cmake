# Ordinary driver test; observer callbacks remain private to its executable.
list(APPEND TEST_SOURCES test_selected_capacity_script_bars)
pineforge_add_unit_test(test_selected_capacity_script_bars)
set_tests_properties(test_selected_capacity_script_bars PROPERTIES LABELS "selected_capacity")
