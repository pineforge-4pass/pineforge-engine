# Capacity args: unchanged-head rows compile, then fail on absent numbers.
list(APPEND TEST_SOURCES test_cr403_capacity_args)
pineforge_add_unit_test(test_cr403_capacity_args)
add_test(NAME test_cr403_capacity_objects COMMAND test_cr403_capacity_args objects)
add_test(NAME test_cr403_capacity_id_pair COMMAND test_cr403_capacity_args id_pair)
add_test(NAME test_cr403_capacity_disk_id COMMAND test_cr403_capacity_args disk_id)
add_test(NAME test_cr403_capacity_resident COMMAND test_cr403_capacity_args resident)
add_test(NAME test_cr403_capacity_roots COMMAND test_cr403_capacity_args roots)
add_test(NAME test_cr403_capacity_slab COMMAND test_cr403_capacity_args slab)
add_test(NAME test_cr403_capacity_layout COMMAND test_cr403_capacity_args layout)
add_test(NAME test_cr403_capacity_legacy COMMAND test_cr403_capacity_args legacy)
add_test(NAME test_cr403_run_failure_catalog
    COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/scripts/test_gen_run_failure_catalog_diff.py
        CapacityCatalog.test_cr403_scratch_capacity_optional_args)
