# The strict-reader runner supplies independent bytes/digests to the wrapped
# executable declared in the matching fence. Each row is red on window syncs.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_report_sink_json_deferred_sync)
    add_test(NAME test_report_sink_json_deferred_sync_partial
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> partial)
    add_test(NAME test_report_sink_json_deferred_sync_exact_equity
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> exact_equity)
    add_test(NAME test_report_sink_json_deferred_sync_exact_trades
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> exact_trades)
    add_test(NAME test_report_sink_json_deferred_sync_source_refusal
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> source_refusal)
    add_test(NAME test_report_sink_json_deferred_sync_unsupported
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> unsupported)
    add_test(NAME test_report_sink_json_deferred_sync_deferred_result
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/report_sink_json_sync_fixture.py
        $<TARGET_FILE:test_report_sink_json_deferred_sync> deferred_result)
endif()
