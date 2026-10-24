if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_disk_resident_id_table_resident_io)
    pineforge_add_unit_test(test_disk_resident_id_table_resident_io)
endif()
