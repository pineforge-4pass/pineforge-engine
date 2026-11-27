if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_chart_ema_generated_cache_seed)
    pineforge_add_unit_test(test_chart_ema_generated_cache_seed)
    list(APPEND TEST_SOURCES test_chart_ema_batch_interior_na)
    pineforge_add_unit_test(test_chart_ema_batch_interior_na)
    list(APPEND TEST_SOURCES test_chart_tsi_batch_origin_seed)
    pineforge_add_unit_test(test_chart_tsi_batch_origin_seed)
    list(APPEND TEST_SOURCES test_chart_ema_batch_security_policy)
    pineforge_add_unit_test(test_chart_ema_batch_security_policy)
endif()

add_test(NAME test_run_strategy_series_origin
    COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/scripts/test_run_strategy_series_origin.py)
