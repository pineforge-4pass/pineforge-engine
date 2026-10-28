# Replay S-record red-first tests.
add_test(NAME test_replay_run_json
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_replay_test.py)
add_test(NAME test_replay_sink
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/pineforge_sink_replay_test.py)

if(UNIX)
    list(APPEND TEST_SOURCES test_replay_abi)
    pineforge_add_unit_test(test_replay_abi)
endif()

# Same real-engine orders through the production JSON CLI. The fixture module
# is defined in this lane's fence; Python cannot load instrumented modules.
if(UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_replay_run_json_engine
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_LIBRARY=$<TARGET_FILE:replay_strategy>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_replay_engine.py)
endif()
