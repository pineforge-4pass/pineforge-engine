cmake_minimum_required(VERSION 3.16)

# Only the sibling twin executable is an input to this control.
set(forbidden "(^|/)(\\.ssh|\\.aws|\\.codex|\\.config/(zsh/keys|pineforge))(/|$)|\\.env(/|$)")
if(NOT IS_ABSOLUTE "${TWIN}" OR TWIN MATCHES "${forbidden}"
   OR NOT TWIN MATCHES "/test_native_instrument_feed_twin$")
    message(FATAL_ERROR "Refused twin path: ${TWIN}")
endif()
message(STATUS "Twin control input: ${TWIN}")
get_filename_component(child "${TWIN}" REALPATH)
if(NOT child MATCHES "/test_native_instrument_feed_twin$"
   OR child MATCHES "${forbidden}")
    message(FATAL_ERROR "Refused twin path: ${TWIN} -> ${child}")
endif()
if(NOT EXISTS "${child}")
    message(FATAL_ERROR "NOT COMPARED: expected built twin; actual missing '${child}'")
endif()
message(STATUS "Twin control executes ${child}")

function(read_child mode)
    execute_process(COMMAND "${child}" "${mode}" RESULT_VARIABLE rc
                    OUTPUT_VARIABLE output ERROR_VARIABLE error)
    message("${mode}: exit=${rc}\n${output}${error}")
    set(child_rc "${rc}" PARENT_SCOPE)
    set(child_out "${output}" PARENT_SCOPE)
    set(child_err "${error}" PARENT_SCOPE)
endfunction()

# Exact complete diagnostic line and exactly one counted failure from the
# ordinary run helper. No setup failure or abnormal termination qualifies.
function(matches_refusal reason result)
    string(FIND "${child_out}" "\nchild: run=refused failures=1 error=${reason}\n" at)
    if(child_rc STREQUAL "1" AND NOT at EQUAL -1)
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(missing "request.security: no feed is installed for symbol 'SYN:US10Y' at timeframe '60'")
read_child(--missing-feed)
matches_refusal("${missing}" matched)
if(NOT matched)
    message(FATAL_ERROR "Expected counted missing-feed refusal; actual exit=${child_rc}, output above")
endif()

read_child(--invalid-symbol)
matches_refusal("${missing}" matched)
matches_refusal("request.security: symbol 'SYN:US10Y' is invalid" wrong_cause)
if(matched OR NOT wrong_cause)
    message(FATAL_ERROR "Expected a counted invalid-symbol refusal rejected by the missing-feed check")
endif()

# A signal is not a normal test failure, even if a process printed a reason.
read_child(--crash-control)
matches_refusal("${missing}" matched)
if(matched OR child_rc MATCHES "^[0-9]+$" OR NOT child_err MATCHES "twin crash control: aborting")
    message(FATAL_ERROR "Expected the crash control's abnormal termination; actual ${child_rc}")
endif()
