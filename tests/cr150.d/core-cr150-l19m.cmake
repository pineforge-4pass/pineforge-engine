# CR-150 L19m: the census's empty exposure capacity and closed-trade phases.
if(UNIX)
    list(APPEND TEST_SOURCES test_adapter_growth_storage)
    pineforge_add_unit_test(test_adapter_growth_storage)
endif()
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_adapter_phase_disk)
    pineforge_add_unit_test(test_adapter_phase_disk)
endif()
