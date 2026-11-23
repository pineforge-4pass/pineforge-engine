if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_journal_enum_membership)
    pineforge_add_unit_test(test_journal_enum_membership)
endif()
