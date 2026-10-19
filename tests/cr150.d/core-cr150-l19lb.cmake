# CR-150 L19l part B: kernel roster identity storage and reference/heap proof.
list(APPEND TEST_SOURCES test_kernel_cohort_rosters)
pineforge_add_unit_test(test_kernel_cohort_rosters)
list(APPEND TEST_SOURCES test_kernel_roster_store)
pineforge_add_unit_test(test_kernel_roster_store)
