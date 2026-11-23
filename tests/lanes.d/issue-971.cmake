if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admission_nonfill_birth)
    pineforge_add_unit_test(test_admission_nonfill_birth)
endif()
