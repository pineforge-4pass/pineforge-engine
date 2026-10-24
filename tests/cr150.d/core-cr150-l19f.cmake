# CR-150 L19F: K5 retirement in script-bar space (F0b).
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_k5_script_bar_retirement)
    pineforge_add_unit_test(test_k5_script_bar_retirement)
endif()
