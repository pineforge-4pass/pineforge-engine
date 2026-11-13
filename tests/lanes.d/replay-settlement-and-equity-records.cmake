# Replay R/Q: the C++ row proves fixture eligibility on the old engine;
# Python rows are red-first. No environment-dependent skips.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_replay_rq_fixture)
    pineforge_add_unit_test(test_replay_rq_fixture)
endif()
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_replay_rq_guards COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_RQ_LIBRARY=$<TARGET_FILE:replay_rq_strategy>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/replay_rq_engine_test.py -k test_guard)
    add_test(NAME test_replay_rq_vectors COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_RQ_LIBRARY=$<TARGET_FILE:replay_rq_strategy>" "PINEFORGE_REPLAY_RQ_PLANNER=$<TARGET_FILE:pineforge_window_plan>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/replay_rq_engine_test.py -k "not test_guard")
    add_test(NAME test_replay_rq_companion COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_RQ_LIBRARY=$<TARGET_FILE:replay_rq_strategy>" "PINEFORGE_REPLAY_RQ_PLANNER=$<TARGET_FILE:pineforge_window_plan>" ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/pineforge_sink_replay_rq_test.py)
    set_tests_properties(test_replay_rq_guards test_replay_rq_vectors test_replay_rq_companion PROPERTIES LABELS "replay_rq;replay" DEPENDS test_replay_rq_fixture)
    # Default-off byte pin: the default JSON and the primary sink stream of three
    # fixed scripts, compared as raw bytes with digests recorded from main.
    add_test(NAME test_replay_off_golden COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_CANDIDATE_LIBRARY=$<TARGET_FILE:replay_rq_strategy>" ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/check_replay_off_golden.py)
    set_tests_properties(test_replay_off_golden PROPERTIES LABELS "replay;off_bytes" DEPENDS test_replay_rq_fixture)
    # Configure -DPINEFORGE_REPLAY_BASELINE_BUILD:PATH=... only after the CI
    # replay-baseline step described in docs/replay-ci-README.md.
    if(PINEFORGE_REPLAY_BASELINE_BUILD)
        add_test(NAME test_replay_off_identity COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_REPLAY_BASELINE_BUILD=${PINEFORGE_REPLAY_BASELINE_BUILD}" "PINEFORGE_REPLAY_CANDIDATE_LIBRARY=$<TARGET_FILE:replay_rq_strategy>" ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/check_replay_off_identity.py)
        set_tests_properties(test_replay_off_identity PROPERTIES LABELS "replay;c4" DEPENDS test_replay_rq_fixture RUN_SERIAL TRUE)
        set_tests_properties(test_replay_rq_vectors test_replay_rq_companion PROPERTIES DEPENDS "test_replay_rq_fixture;test_replay_off_identity")
    endif()
endif()
