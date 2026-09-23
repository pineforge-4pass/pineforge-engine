# R5 lane PERF-L4: the differential row of the Pine adapter's quiet-bar gates.
#
# GATED is test_adapter_quiet_bar on the shipped library, UNGATED the same TU on
# the source layer compiled without a gate. Each prints its battery bar by bar
# (`transcript`): every source-layer fold, recorded broker-state hash row,
# final hash, continuation and trade row of every run. The two must be the
# same, line for line.
foreach(_required GATED UNGATED WORK_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "quiet_bar_differential.cmake: ${_required} is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${WORK_DIR}")
set(_gated "${WORK_DIR}/gated.txt")
set(_ungated "${WORK_DIR}/ungated.txt")
execute_process(COMMAND "${GATED}" transcript OUTPUT_FILE "${_gated}"
                RESULT_VARIABLE _gated_result)
execute_process(COMMAND "${UNGATED}" transcript OUTPUT_FILE "${_ungated}"
                RESULT_VARIABLE _ungated_result)
if(NOT _gated_result EQUAL 0 OR NOT _ungated_result EQUAL 0)
    message(FATAL_ERROR "quiet_bar_differential: transcript runs failed "
                        "(gated ${_gated_result}, ungated ${_ungated_result})")
endif()
file(STRINGS "${_gated}" _gated_lines)
file(STRINGS "${_ungated}" _ungated_lines)
list(LENGTH _gated_lines _gated_count)
list(LENGTH _ungated_lines _ungated_count)
if(_gated_count EQUAL 0)
    message(FATAL_ERROR "quiet_bar_differential: the gated transcript is empty")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${_gated}" "${_ungated}"
                RESULT_VARIABLE _differ)
if(_differ)
    set(_line 0)
    foreach(_gated_line IN LISTS _gated_lines)
        if(_line GREATER_EQUAL _ungated_count)
            break()
        endif()
        list(GET _ungated_lines ${_line} _ungated_line)
        if(NOT _gated_line STREQUAL _ungated_line)
            break()
        endif()
        math(EXPR _line "${_line} + 1")
    endforeach()
    math(EXPR _shown "${_line} + 1")
    message(FATAL_ERROR "quiet_bar_differential: the transcripts differ at line ${_shown} "
                        "(gated ${_gated_count} lines, ungated ${_ungated_count}); "
                        "see ${_gated} and ${_ungated}")
endif()
list(FILTER _gated_lines INCLUDE REGEX "^run ")
list(LENGTH _gated_lines _runs)
message("test_adapter_quiet_bar_differential: ${_runs} runs, ${_gated_count} lines, "
        "gated and ungated identical")
