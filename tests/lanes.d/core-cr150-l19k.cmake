# CR-150 L19K: the N-day request.security partition generated, not stored (F2).
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_multi_day_periods_bounded)
    pineforge_add_unit_test(test_multi_day_periods_bounded)
endif()
