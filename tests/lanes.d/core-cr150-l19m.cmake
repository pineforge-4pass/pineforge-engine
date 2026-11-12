# CR-150 L19m: the census's empty exposure capacity in real settlements.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_adapter_growth_storage)
    pineforge_add_unit_test(test_adapter_growth_storage)
    list(APPEND TEST_SOURCES test_adapter_reversal_storage)
    pineforge_add_unit_test(test_adapter_reversal_storage)
endif()
