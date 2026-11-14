# Admission journal base.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admission_journal_window)
    pineforge_add_unit_test(test_admission_journal_window)
endif()
