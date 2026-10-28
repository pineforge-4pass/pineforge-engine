# Run one planted pipe-writer mode and require the guard's normal failure.
#
# CTest's PASS_REGULAR_EXPRESSION takes precedence over the child exit result,
# and WILL_FAIL treats every nonzero child result as a pass. This driver keeps
# both assertions: the child must return exactly 1 and print the guard message
# for the boundary belonging to the selected plant.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PIPE_WRITER OR PIPE_WRITER STREQUAL ""
        OR NOT DEFINED PLANT_MODE OR PLANT_MODE STREQUAL "")
    message(FATAL_ERROR
        "run_pipe_writer_fd_flags.cmake requires PIPE_WRITER and PLANT_MODE")
endif()

if(PLANT_MODE STREQUAL "--plant-nonblock")
    set(_pf_expected_boundary "after planted F_SETFL")
elseif(PLANT_MODE STREQUAL "--plant-close")
    set(_pf_expected_boundary "after planted close")
else()
    message(FATAL_ERROR "unsupported pipe writer plant mode: ${PLANT_MODE}")
endif()

execute_process(
    COMMAND "${PIPE_WRITER}" "${PLANT_MODE}"
    RESULT_VARIABLE _pf_result
    OUTPUT_VARIABLE _pf_stdout
    ERROR_VARIABLE _pf_stderr)

if(NOT _pf_stdout STREQUAL "")
    message("${_pf_stdout}")
endif()
if(NOT _pf_stderr STREQUAL "")
    message("${_pf_stderr}")
endif()

if(NOT _pf_result STREQUAL "1")
    message(FATAL_ERROR
        "${PLANT_MODE}: expected normal exit 1, got ${_pf_result}")
endif()

set(_pf_guard_regex
    "borrowed fd invariant: .*first_difference=\"${_pf_expected_boundary}\"")
if(NOT _pf_stdout MATCHES "${_pf_guard_regex}"
        AND NOT _pf_stderr MATCHES "${_pf_guard_regex}")
    message(FATAL_ERROR
        "${PLANT_MODE}: missing guard rejection for ${_pf_expected_boundary}")
endif()
