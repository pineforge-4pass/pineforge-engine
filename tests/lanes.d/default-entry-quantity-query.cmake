# Source-host default entry quantity observation unit test.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_default_entry_qty)
    pineforge_add_unit_test(test_default_entry_qty)
endif()
