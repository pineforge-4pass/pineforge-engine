if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_native_cohort_target_invalidation)
    pineforge_add_unit_test(test_native_cohort_target_invalidation)
endif()
