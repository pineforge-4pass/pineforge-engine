if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
add_test(NAME test_report_sink_trade_reference
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_report_sink_trade_reference.py)
add_test(NAME test_report_sink_trade_comparison
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_report_sink_trade_comparison.py)
set_tests_properties(test_report_sink_trade_reference test_report_sink_trade_comparison
    PROPERTIES LABELS "slow" RUN_SERIAL TRUE)
endif()
