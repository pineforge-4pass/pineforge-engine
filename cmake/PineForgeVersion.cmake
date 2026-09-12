# PineForgeVersion.cmake
#
# PINEFORGE_VERSION_SOURCE (CACHE STRING, AUTO|FILE):
#   AUTO (default) — historical behavior exactly: prefer
#     `git describe --tags --match 'v*' --abbrev=7 --dirty` for MMP/FULL
#     when it matches MAJOR.MINOR.PATCH after stripping a leading 'v';
#     otherwise use the VERSION file. Git SHA/dirty are observed the same way.
#   FILE — MMP and FULL always come from VERSION (stable release identity).
#     Git SHA/dirty are still observed as separate fields; checkout depth or
#     missing tags cannot change MMP/FULL. Invalid values fail configure.
#
# Sets in the calling scope:
#   PINEFORGE_VERSION_MAJOR   integer
#   PINEFORGE_VERSION_MINOR   integer
#   PINEFORGE_VERSION_PATCH   integer
#   PINEFORGE_VERSION_MMP     "MAJOR.MINOR.PATCH"  (for project(VERSION ...))
#   PINEFORGE_VERSION_FULL    AUTO: describe-or-VERSION; FILE: VERSION
#   PINEFORGE_VERSION_GIT_SHA short sha or "unknown"
#   PINEFORGE_VERSION_DIRTY   ON/OFF

set(PINEFORGE_VERSION_SOURCE "AUTO" CACHE STRING
    "Version identity source: AUTO (git describe preferred) or FILE (VERSION MMP/FULL)")
set_property(CACHE PINEFORGE_VERSION_SOURCE PROPERTY STRINGS AUTO FILE)

function(_pineforge_read_version_file _out_mmp)
    set(_vfile "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    if(NOT EXISTS "${_vfile}")
        message(FATAL_ERROR "VERSION file missing at ${_vfile}")
    endif()
    file(READ "${_vfile}" _raw)
    string(STRIP "${_raw}" _raw)
    if(NOT _raw MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "VERSION file must be MAJOR.MINOR.PATCH (got '${_raw}')")
    endif()
    set(${_out_mmp} "${_raw}" PARENT_SCOPE)
endfunction()

function(pineforge_resolve_version)
    if(NOT PINEFORGE_VERSION_SOURCE STREQUAL "AUTO" AND
       NOT PINEFORGE_VERSION_SOURCE STREQUAL "FILE")
        message(FATAL_ERROR
            "PINEFORGE_VERSION_SOURCE must be AUTO or FILE (got '${PINEFORGE_VERSION_SOURCE}')")
    endif()

    _pineforge_read_version_file(_file_mmp)

    set(_git_full "")
    set(_git_sha "unknown")
    set(_dirty OFF)

    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v*" --abbrev=7 --dirty
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _desc
            ERROR_QUIET
            RESULT_VARIABLE _rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_rc EQUAL 0 AND _desc)
            # Strip leading 'v'
            string(REGEX REPLACE "^v" "" _git_full "${_desc}")
            if(_git_full MATCHES "-dirty$")
                set(_dirty ON)
            endif()
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _sha_out
            ERROR_QUIET
            RESULT_VARIABLE _sha_rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_sha_rc EQUAL 0 AND _sha_out)
            set(_git_sha "${_sha_out}")
        endif()
    endif()

    if(PINEFORGE_VERSION_SOURCE STREQUAL "FILE")
        set(_mmp "${_file_mmp}")
        set(_full "${_file_mmp}")
    else()
        set(_mmp "${_file_mmp}")
        if(_git_full MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
            set(_mmp "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
        endif()
        if(NOT _git_full)
            set(_git_full "${_mmp}")
        endif()
        set(_full "${_git_full}")
    endif()

    string(REPLACE "." ";" _parts "${_mmp}")
    list(GET _parts 0 _maj)
    list(GET _parts 1 _min)
    list(GET _parts 2 _pat)

    set(PINEFORGE_VERSION_MAJOR   "${_maj}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_MINOR   "${_min}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_PATCH   "${_pat}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_MMP     "${_mmp}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_FULL    "${_full}"     PARENT_SCOPE)
    set(PINEFORGE_VERSION_GIT_SHA "${_git_sha}"  PARENT_SCOPE)
    set(PINEFORGE_VERSION_DIRTY   "${_dirty}"    PARENT_SCOPE)
endfunction()
