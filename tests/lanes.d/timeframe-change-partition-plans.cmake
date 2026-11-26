# timeframe.change plans each N-day partition once per run, however many timeframes a script names.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_timeframe_change_partition_plans)
    pineforge_add_unit_test(test_timeframe_change_partition_plans)
endif()
