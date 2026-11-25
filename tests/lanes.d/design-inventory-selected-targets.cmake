add_test(NAME test_design_inventory_explicit_unit
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_check_design_inventory.py
            SelectedRegistrations.test_explicit_unit_registration_after_the_source_list_loop)
add_test(NAME test_design_inventory_module_provider
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_check_design_inventory.py
            SelectedRegistrations.test_script_registration_uses_the_selected_module_provider)
