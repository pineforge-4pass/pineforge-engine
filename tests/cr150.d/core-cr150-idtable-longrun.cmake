if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_source_resident_cohort_longrun)
    pineforge_add_unit_test(test_source_resident_cohort_longrun)
    set_tests_properties(test_source_resident_cohort_longrun PROPERTIES LABELS slow RUN_SERIAL TRUE)

    list(APPEND TEST_SOURCES test_resident_cohort_sized_gap)
    pineforge_add_unit_test(test_resident_cohort_sized_gap)
    set_tests_properties(test_resident_cohort_sized_gap PROPERTIES LABELS slow RUN_SERIAL TRUE)
endif()
