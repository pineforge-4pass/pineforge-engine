if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_entry_quantity_json
        COMMAND ${CMAKE_COMMAND} -E env
            "PINEFORGE_ENTRY_PERCENT_LIBRARY=$<TARGET_FILE:entry_quantity_percent>"
            "PINEFORGE_ENTRY_EXPLICIT_LIBRARY=$<TARGET_FILE:entry_quantity_explicit>"
            ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
            ${PROJECT_SOURCE_DIR}/tests/test_entry_quantity_callers.py::test_json_entry_quantity)
    if(PINEFORGE_BUILD_LIVE_RUNNER)
        add_test(NAME test_entry_quantity_live
            COMMAND ${CMAKE_COMMAND} -E env
                "PINEFORGE_ENTRY_PERCENT_LIBRARY=$<TARGET_FILE:entry_quantity_percent>"
                "PINEFORGE_ENTRY_EXPLICIT_LIBRARY=$<TARGET_FILE:entry_quantity_explicit>"
                "PINEFORGE_ENTRY_LIVE_RUNNER=$<TARGET_FILE:pineforge-live>"
                ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
                ${PROJECT_SOURCE_DIR}/tests/test_entry_quantity_callers.py::test_live_entry_quantity)
    endif()
endif()
