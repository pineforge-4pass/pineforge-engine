
# --- CR-150 core-cr150-l9a begin ---
# Full JSON through the temporary sink; after every line pin.
# Preflight also discovers this file through docker/run_json_*_test.py.
add_test(NAME test_run_json_sink_l9a
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_sink.py)
if(TARGET report_sink_l6b1_strategy AND NOT PINEFORGE_ENABLE_SANITIZERS
   AND NOT PINEFORGE_ENABLE_COVERAGE)
    set_property(TEST test_run_json_sink_l9a APPEND PROPERTY ENVIRONMENT
        "PINEFORGE_L9A_STRATEGY_SO=$<TARGET_FILE:report_sink_l6b1_strategy>")
endif()
if(TARGET outputs_fixture_strategy)
    set_property(TEST test_run_json_sink_l9a APPEND PROPERTY ENVIRONMENT
        "PINEFORGE_L9A_OUTPUTS_SO=$<TARGET_FILE:outputs_fixture_strategy>")
endif()
# --- CR-150 core-cr150-l9a end ---
