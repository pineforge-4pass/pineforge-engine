# CR-150 L19l part C: auxiliary disk tables and the real adapter differential.
if(UNIX)
    list(APPEND TEST_SOURCES test_id_pair_store)
    pineforge_add_unit_test(test_id_pair_store)
endif()
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_adapter_auxiliary_disk)
    pineforge_add_unit_test(test_adapter_auxiliary_disk)
endif()
