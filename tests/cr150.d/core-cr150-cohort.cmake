# CR-347 round 5: PR #458's fresh-id reproducer, credited in the test body.
# Only a Release library supplies a timing proof; debug exactness assertions
# deliberately walk all residents. The existing executable keeps its old rows.
if(PINEFORGE_BUILD_SOURCE_LAYER AND TARGET test_adapter_live_state_scaling)
    add_test(NAME test_adapter_spaced_ids_scaling
        COMMAND ${CMAKE_COMMAND} -E env
            PINEFORGE_V19E_SCALING_GATE=$<IF:$<CONFIG:Release>,1,0>
            ${CMAKE_COMMAND}
                "-DCR347_SCALING_EXECUTABLE=$<TARGET_FILE:test_adapter_live_state_scaling>"
                -P "${CMAKE_CURRENT_LIST_DIR}/../run_adapter_spaced_ids_scaling.cmake")
    set_tests_properties(test_adapter_spaced_ids_scaling PROPERTIES LABELS "slow" RUN_SERIAL TRUE)
endif()
