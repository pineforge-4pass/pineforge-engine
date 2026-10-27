# Deferred sink sync: Linux --wrap accounting of the path sink's one deferred sync.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_report_sink_deferred_sync)
    pineforge_add_unit_test(test_report_sink_deferred_sync)
endif()
