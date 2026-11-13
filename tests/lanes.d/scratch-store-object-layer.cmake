list(APPEND TEST_SOURCES test_o1_store)
pineforge_add_unit_test(test_o1_store)
set_tests_properties(test_o1_store PROPERTIES LABELS "slow;mem")
