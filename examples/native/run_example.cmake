# Runs one examples/native host for its example_* CTest row. The row asserts
# two things: the host exits 0, and its output (stdout and stderr merged, as
# CTest merges them) matches EXPECT, the summary line the host prints only
# after every check of its own has passed.
#
# CTest cannot assert both on one row: once PASS_REGULAR_EXPRESSION is set the
# exit code is ignored, so a host that printed its summary line and then
# exited nonzero passed. Here a nonzero exit, a signal, a timeout or a missing
# line each fails the row, after the host's own output has been echoed.
#
#   cmake -DEXAMPLE=<host> -DEXPECT=<regex> [-DTIMEOUT=<seconds>] -P run_example.cmake
#
# No timeout is applied unless the caller passes -DTIMEOUT: with the row's own
# CTest TIMEOUT property gone (owner rule, 2026-10-12) the host runs until it
# finishes, and only a hang reaches the platform's job maximum. The mutation
# self-test below passes -DTIMEOUT=1 to exercise the hang path.
#
# scripts/test_example_runner.py is this script's mutation self-test.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED EXAMPLE OR EXAMPLE STREQUAL "" OR NOT DEFINED EXPECT OR EXPECT STREQUAL "")
    message(FATAL_ERROR "run_example.cmake: pass -DEXAMPLE=<host> and -DEXPECT=<regex>")
endif()
if(DEFINED TIMEOUT AND NOT TIMEOUT STREQUAL "")
    set(_pf_timeout_args TIMEOUT "${TIMEOUT}")
else()
    set(_pf_timeout_args "")
endif()

execute_process(COMMAND "${EXAMPLE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
    ${_pf_timeout_args})
if(NOT output STREQUAL "")
    message("${output}")
endif()
if(NOT status STREQUAL "0")
    message(FATAL_ERROR
        "${EXAMPLE} did not exit 0 (${status}); a summary line printed before that does not count")
endif()
if(NOT output MATCHES "${EXPECT}")
    message(FATAL_ERROR "${EXAMPLE} exited 0 but printed no line matching \"${EXPECT}\"")
endif()
