# CR150 A4 P26-P41: populations, ordered booking and bounded readback.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_a4_ordered_inventory)
    pineforge_add_unit_test(test_a4_ordered_inventory)
    list(APPEND TEST_SOURCES test_a4_admission_inventory)
    pineforge_add_unit_test(test_a4_admission_inventory)
endif()
