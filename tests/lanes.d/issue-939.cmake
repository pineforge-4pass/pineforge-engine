if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admission_journal_tag_read_status)
    pineforge_add_unit_test(test_admission_journal_tag_read_status)
endif()
