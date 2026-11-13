add_test(NAME test_run_json_run_warnings
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/test_run_json_run_warnings.py -k "not native"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_run_warnings_native
        COMMAND ${CMAKE_COMMAND} -E env
            "PINEFORGE_RUN_WARNINGS_STRATEGY_SO=$<TARGET_FILE:run_json_run_warnings_strategy>"
            "PINEFORGE_NO_RUN_WARNINGS_STRATEGY_SO=$<TARGET_FILE:run_json_no_run_warnings_strategy>"
            "PINEFORGE_RUN_WARNINGS_PLANNER_SO=$<TARGET_FILE:pineforge_window_plan>"
            ${Python3_EXECUTABLE} -B -m pytest -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_run_json_run_warnings.py -k native
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
