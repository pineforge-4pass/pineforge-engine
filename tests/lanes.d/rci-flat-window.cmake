if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_rci_flat_window)
    pineforge_add_unit_test(test_rci_flat_window)
endif()
