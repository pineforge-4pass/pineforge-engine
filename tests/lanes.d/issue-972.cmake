if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_journal_append_stored_record)
    pineforge_add_unit_test(test_journal_append_stored_record)
endif()
