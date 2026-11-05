if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_ledger_cache_policy
        COMMAND ${CMAKE_COMMAND} -E env
            "PF_LEDGER_CACHE_POLICY_FIXTURE=$<TARGET_FILE:run_json_ledger_cache_policy_fixture>"
            ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_ledger_cache_policy.py)
    set_tests_properties(test_run_json_ledger_cache_policy PROPERTIES LABELS "slow;mem")
endif()
