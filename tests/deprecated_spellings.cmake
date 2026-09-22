# R5 lane F6 item 4: the P2c historical spellings are compiler-deprecated.
#
# ADR-0001 "Deprecated public spellings" keeps four old names as
# value-identical aliases until their ABI's next epoch:
# pf_equity_stats_t::sharpe_tv / sortino_tv (public C ABI, PF_ABI_VERSION 5)
# and exit_legs::Domain::Coof / MagnifierCoof (lifecycle_v2). Until this
# lane the deprecation was documentation only: a consumer compiled the old
# names silently. This row holds the three facts a consumer relies on:
#
#   1. the generic spellings compile clean under
#      -Werror=deprecated-declarations, in C (C11) and in C++ (C++17);
#   2. each historical spelling draws the compiler's deprecation
#      diagnostic, so under -Werror=deprecated-declarations the same probe
#      fails, naming it;
#   3. with -Wno-deprecated-declarations the historical spellings still
#      compile: an alias, never a break (the twin-parity-frozen suites that
#      keep their base CHECK texts build exactly that way).
#
#   cmake -DPF_CC=<cc> -DPF_CXX=<c++> -DPF_INCLUDES=<dir|dir>
#         -DPF_SOURCE_DIR=<tests dir> -DPF_WORK_DIR=<scratch> -P deprecated_spellings.cmake

cmake_minimum_required(VERSION 3.16)

foreach(required PF_CC PF_CXX PF_INCLUDES PF_SOURCE_DIR PF_WORK_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "deprecated_spellings.cmake: pass -D${required}=...")
    endif()
endforeach()
file(MAKE_DIRECTORY "${PF_WORK_DIR}")
# `|`-separated: a `;` would not survive add_test's argument list.
string(REPLACE "|" ";" include_dirs "${PF_INCLUDES}")
set(include_flags)
foreach(dir IN LISTS include_dirs)
    list(APPEND include_flags "-I${dir}")
endforeach()

set(failures 0)
function(compile_probe label compiler standard source extra_flags out_status out_text)
    execute_process(
        COMMAND "${compiler}" "${standard}" -fsyntax-only ${include_flags}
                ${extra_flags} "${PF_SOURCE_DIR}/${source}"
        WORKING_DIRECTORY "${PF_WORK_DIR}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE text
        ERROR_VARIABLE text
        TIMEOUT 120)
    set(${out_status} "${status}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

foreach(language C CXX)
    if(language STREQUAL "C")
        set(compiler "${PF_CC}")
        set(standard "-std=c11")
        set(source deprecated_spellings_probe.c)
        set(names sharpe_tv sortino_tv)
    else()
        set(compiler "${PF_CXX}")
        set(standard "-std=c++17")
        set(source deprecated_spellings_probe.cpp)
        set(names Coof MagnifierCoof sharpe_tv sortino_tv)
    endif()

    compile_probe(generic "${compiler}" "${standard}" "${source}"
        "-Werror=deprecated-declarations" status text)
    if(NOT status STREQUAL "0")
        message("${text}")
        message(SEND_ERROR "${language}: the generic spellings do not compile clean "
                           "under -Werror=deprecated-declarations (${status})")
        math(EXPR failures "${failures} + 1")
    else()
        message(STATUS "${language}: generic spellings compile clean")
    endif()

    compile_probe(historical "${compiler}" "${standard}" "${source}"
        "-Werror=deprecated-declarations;-DPF_PROBE_HISTORICAL" status text)
    if(status STREQUAL "0")
        message(SEND_ERROR "${language}: the historical spellings (${names}) compiled "
                           "under -Werror=deprecated-declarations with no deprecation "
                           "diagnostic")
        math(EXPR failures "${failures} + 1")
    else()
        string(REPLACE "\n" ";" lines "${text}")
        foreach(name IN LISTS names)
            set(found FALSE)
            foreach(line IN LISTS lines)
                if(line MATCHES "(^|[^A-Za-z0-9_])${name}([^A-Za-z0-9_]|$)"
                   AND line MATCHES "deprecated")
                    set(found TRUE)
                    break()
                endif()
            endforeach()
            if(NOT found)
                message("${text}")
                message(SEND_ERROR "${language}: no deprecation diagnostic names ${name}")
                math(EXPR failures "${failures} + 1")
            else()
                message(STATUS "${language}: ${name} is deprecated")
            endif()
        endforeach()
    endif()

    compile_probe(escaped "${compiler}" "${standard}" "${source}"
        "-Wno-deprecated-declarations;-DPF_PROBE_HISTORICAL" status text)
    if(NOT status STREQUAL "0")
        message("${text}")
        message(SEND_ERROR "${language}: the historical spellings no longer compile "
                           "with -Wno-deprecated-declarations (${status}): an alias "
                           "was broken")
        math(EXPR failures "${failures} + 1")
    else()
        message(STATUS "${language}: historical spellings still compile as aliases")
    endif()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "deprecated public spellings: ${failures} failure(s)")
endif()
message(STATUS "deprecated public spellings: generic clean, historical deprecated, aliases intact")
