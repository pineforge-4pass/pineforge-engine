if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
list(APPEND TEST_SOURCES test_report_sink_trade_retirement)
pineforge_add_unit_test(test_report_sink_trade_retirement)
list(APPEND TEST_SOURCES test_report_sink_trade_history)
pineforge_add_unit_test(test_report_sink_trade_history)
list(APPEND TEST_SOURCES test_report_sink_trade_legacy)
pineforge_add_unit_test(test_report_sink_trade_legacy)
add_test(NAME test_report_sink_trade_identity
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_report_sink_trade_identity.py
        $<TARGET_FILE:test_report_sink_trade_retirement>
        $<TARGET_FILE:test_report_sink_trade_history>
        $<TARGET_FILE:test_report_sink_trade_legacy>)
add_test(NAME test_sink_history_consumers
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_sink_history_consumers.py)
set_tests_properties(
    test_report_sink_trade_retirement
    test_report_sink_trade_history
    test_report_sink_trade_legacy
    test_report_sink_trade_identity
    test_sink_history_consumers
    PROPERTIES LABELS "slow;mem;LAB-DISKSCRATCH" RUN_SERIAL TRUE)
endif()
