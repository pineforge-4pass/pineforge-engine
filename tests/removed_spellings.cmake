# Lane REL10: the four public spellings ADR-0001 "Deprecated public spellings"
# scheduled for removal are gone at 1.0 -- pf_equity_stats_t::sharpe_tv /
# sortino_tv (the public C header) and pineforge::exit_legs::Domain::Coof /
# MagnifierCoof (the standalone lifecycle_v1 C++ header). This row holds:
#
#   1. the 1.0 spellings (sharpe_monthly, sortino_monthly, FillRecalc,
#      MagnifierFillRecalc) compile clean under -Werror, in strict C99
#      (-pedantic-errors) and in C++17;
#   2. each removed spelling, compiled alone, fails with the compiler's
#      unknown-member diagnostic that names it. Deprecation warnings are
#      suppressed for those compiles, so a spelling that was still declared,
#      deprecated or not, would compile and fail this row.
#
#   cmake -DPF_CC=<cc> -DPF_CXX=<c++> -DPF_INCLUDES=<dir|dir>
#         -DPF_SOURCE_DIR=<tests dir> -DPF_WORK_DIR=<scratch> -P removed_spellings.cmake

cmake_minimum_required(VERSION 3.16)

foreach(required PF_CC PF_CXX PF_INCLUDES PF_SOURCE_DIR PF_WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "removed_spellings.cmake: pass -D${required}=...")
    endif()
endforeach()
file(MAKE_DIRECTORY "${PF_WORK_DIR}")
# `|`-separated: a `;` would not survive add_test's argument list.
string(REPLACE "|" ";" include_dirs "${PF_INCLUDES}")
set(include_flags)
foreach(dir IN LISTS include_dirs)
    list(APPEND include_flags "-I${dir}")
endforeach()

function(compile_probe compiler source out_status out_text)
    execute_process(
        COMMAND "${compiler}" ${ARGN} -fsyntax-only ${include_flags}
                "${PF_SOURCE_DIR}/${source}"
        WORKING_DIRECTORY "${PF_WORK_DIR}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE text
        ERROR_VARIABLE text
        TIMEOUT 120)
    set(${out_status} "${status}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

set(failures 0)
foreach(language C CXX)
    if(language STREQUAL "C")
        set(compiler "${PF_CC}")
        set(flags -std=c99 -pedantic-errors)
        set(source removed_spellings_probe.c)
        set(names sharpe_tv sortino_tv)
    else()
        set(compiler "${PF_CXX}")
        set(flags -std=c++17)
        set(source removed_spellings_probe.cpp)
        set(names Coof MagnifierCoof sharpe_tv sortino_tv)
    endif()

    compile_probe("${compiler}" "${source}" status text ${flags} -Werror)
    if(NOT status STREQUAL "0")
        message("${text}")
        message(SEND_ERROR "${language}: the 1.0 spellings do not compile clean "
                           "under -Werror (${status})")
        math(EXPR failures "${failures} + 1")
    else()
        message(STATUS "${language}: the 1.0 spellings compile clean")
    endif()

    foreach(name IN LISTS names)
        compile_probe("${compiler}" "${source}" status text ${flags}
                      -Wno-deprecated-declarations "-DPF_PROBE_${name}")
        set(found FALSE)
        if(NOT status STREQUAL "0")
            string(REPLACE "\n" ";" lines "${text}")
            foreach(line IN LISTS lines)
                # Clang: "no member named 'X' in ..."; GCC: "... has no member
                # named 'X'" or "'X' is not a member of ...", its quotes
                # typographic in a UTF-8 locale.
                if(line MATCHES "(no member named|is not a member of)"
                   AND line MATCHES "(^|[^A-Za-z0-9_])${name}([^A-Za-z0-9_]|$)")
                    set(found TRUE)
                    break()
                endif()
            endforeach()
        endif()
        if(NOT found)
            message("${text}")
            message(SEND_ERROR "${language}: the removed spelling ${name} did not fail "
                               "with an unknown-member diagnostic naming it (${status})")
            math(EXPR failures "${failures} + 1")
        else()
            message(STATUS "${language}: ${name} is gone (unknown member)")
        endif()
    endforeach()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "removed public spellings: ${failures} failure(s)")
endif()
message(STATUS "removed public spellings: the 1.0 spellings compile clean, "
               "all four removed spellings are unknown")
