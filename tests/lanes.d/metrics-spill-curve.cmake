if(UNIX)
    list(APPEND TEST_SOURCES test_metrics_spill_curve)
    pineforge_add_unit_test(test_metrics_spill_curve)
endif()
