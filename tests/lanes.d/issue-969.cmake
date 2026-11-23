if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admission_draft_receipt_append)
    pineforge_add_unit_test(test_admission_draft_receipt_append)
endif()
