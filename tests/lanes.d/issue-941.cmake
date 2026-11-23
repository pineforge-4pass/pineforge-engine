if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue941_journal_partial_word_io)
    pineforge_add_unit_test(test_issue941_journal_partial_word_io)
endif()
