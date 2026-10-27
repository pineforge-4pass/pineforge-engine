# Compile probes follow tests/removed_spellings.cmake: compile a positive
# control first, then require a specific access diagnostic for the negative.
cmake_minimum_required(VERSION 3.16)

foreach(required PF_CXX PF_INCLUDES PF_SOURCE_DIR PF_WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "private_writers.cmake: pass -D${required}=...")
    endif()
endforeach()
file(MAKE_DIRECTORY "${PF_WORK_DIR}")
string(REPLACE "|" ";" include_dirs "${PF_INCLUDES}")
set(include_flags)
foreach(dir IN LISTS include_dirs)
    list(APPEND include_flags "-I${dir}")
endforeach()

set(positive_source cr_private_writers_must_compile.cpp)
set(negative_source cr_private_writers_must_not_compile.cpp)
file(READ "${PF_SOURCE_DIR}/${positive_source}" positive)
file(READ "${PF_SOURCE_DIR}/${negative_source}" negative)
set(forbidden_call "        (void)last_equity_point();\n")
string(REPLACE "${forbidden_call}" "" negative_without_call "${negative}")
string(LENGTH "${positive}" positive_length)
string(LENGTH "${negative}" negative_length)
string(LENGTH "${forbidden_call}" forbidden_call_length)
math(EXPR expected_negative_length "${positive_length} + ${forbidden_call_length}")
if(NOT negative_without_call STREQUAL positive
   OR NOT negative_length EQUAL expected_negative_length)
    message(FATAL_ERROR "private writers: probes must differ by exactly the forbidden call")
endif()

function(compile_probe source out_status out_text)
    execute_process(
        COMMAND "${PF_CXX}" -std=c++17 -Werror -fsyntax-only ${include_flags}
                "${PF_SOURCE_DIR}/${source}"
        WORKING_DIRECTORY "${PF_WORK_DIR}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE text
        ERROR_VARIABLE text)
    file(WRITE "${PF_WORK_DIR}/${source}.log" "status=${status}\n${text}")
    set(${out_status} "${status}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

compile_probe("${positive_source}" status text)
if(NOT status STREQUAL "0")
    message("${text}")
    message(FATAL_ERROR "private writers: test-only friend route did not compile (${status})")
endif()
message(STATUS "private writers: positive control compiles")

compile_probe("${negative_source}" status text)
set(found FALSE)
if(NOT status STREQUAL "0")
    string(REPLACE "\n" ";" lines "${text}")
    foreach(line IN LISTS lines)
        # Clang: "'last_equity_point' is a private member of ...".
        # GCC: "'...last_equity_point()' is private within this context".
        if(line MATCHES "error:"
           AND line MATCHES "(is a private member|is private within this context)"
           AND line MATCHES "(^|[^A-Za-z0-9_])last_equity_point([^A-Za-z0-9_]|$)")
            set(found TRUE)
            break()
        endif()
    endforeach()
endif()
if(NOT found)
    message("${text}")
    message(FATAL_ERROR
        "private writers: generated subclass did not fail with a private-access error naming last_equity_point (${status})")
endif()
message(STATUS "private writers: generated subclass cannot call last_equity_point (private)")
