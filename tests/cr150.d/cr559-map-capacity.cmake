# Real disk-backed rows, registered like the neighbouring strings/maps tests.
if(UNIX)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_default)
    pineforge_add_unit_test(test_cr559_map_capacity_default)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_resident)
    pineforge_add_unit_test(test_cr559_map_capacity_resident)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_limit)
    pineforge_add_unit_test(test_cr559_map_capacity_limit)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_rehash)
    pineforge_add_unit_test(test_cr559_map_capacity_rehash)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_rollback)
    pineforge_add_unit_test(test_cr559_map_capacity_rollback)
    list(APPEND TEST_SOURCES test_cr559_map_capacity_error_context)
    pineforge_add_unit_test(test_cr559_map_capacity_error_context)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        list(APPEND TEST_SOURCES test_cr559_map_capacity_run)
        pineforge_add_unit_test(test_cr559_map_capacity_run)
        list(APPEND TEST_SOURCES test_cr559_map_capacity_calc_on_fill)
        pineforge_add_unit_test(test_cr559_map_capacity_calc_on_fill)
    endif()
    # One million real pairs, including all reads, is deliberately in the full
    # verification population; there is no timeout or expected-failure marker.
    set_tests_properties(test_cr559_map_capacity_resident PROPERTIES LABELS slow)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        # A missing genuine generated.cpp is a configure error, never a skipped
        # row. The pair job generates it before configuring this target.
        add_library(cr559_cli_pair SHARED
            "${PROJECT_SOURCE_DIR}/tests/fixtures/cr559_cli_pair/generated.cpp")
        target_link_libraries(cr559_cli_pair PRIVATE pineforge)
        if(APPLE)
            target_link_options(cr559_cli_pair PRIVATE
                "-Wl,-force_load,$<TARGET_FILE:pineforge>")
        else()
            target_link_options(cr559_cli_pair PRIVATE
                "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
        endif()
        set_target_properties(cr559_cli_pair PROPERTIES PREFIX "")
        add_test(NAME test_cr559_map_capacity_cli
            COMMAND bash "${PROJECT_SOURCE_DIR}/tests/cr559_cli_pair.sh"
                "${PROJECT_SOURCE_DIR}" "$<TARGET_FILE:cr559_cli_pair>"
                "${CMAKE_CURRENT_BINARY_DIR}/cr559-cli-pair")
        set_tests_properties(test_cr559_map_capacity_cli PROPERTIES LABELS slow)
    endif()
endif()
