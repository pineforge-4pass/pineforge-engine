if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pooc_stop_limit_entry_at_close)
    pineforge_add_unit_test(test_pooc_stop_limit_entry_at_close)
endif()
