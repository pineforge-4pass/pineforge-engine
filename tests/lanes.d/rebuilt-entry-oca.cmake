if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_rebuilt_entry_oca)
    pineforge_add_unit_test(test_rebuilt_entry_oca)
endif()
