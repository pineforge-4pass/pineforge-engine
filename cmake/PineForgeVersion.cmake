# PineForgeVersion.cmake
#
# Resolve PineForge version from two sources, in priority order:
#   1. `git describe --tags --match 'v*' --abbrev=7 --dirty` (when in a git
#      checkout). Strips leading 'v', exposes commits-since-tag + sha + dirty
#      as PINEFORGE_VERSION_FULL.
#   2. The `VERSION` file at the repo root (always present in tarballs and
#      Docker build contexts).
#
# Sets in the calling scope:
#   PINEFORGE_VERSION_MAJOR   integer
#   PINEFORGE_VERSION_MINOR   integer
#   PINEFORGE_VERSION_PATCH   integer
#   PINEFORGE_VERSION_MMP     "MAJOR.MINOR.PATCH"  (for project(VERSION ...))
#   PINEFORGE_VERSION_FULL    "MAJOR.MINOR.PATCH[-N-gSHA[-dirty]]"
#   PINEFORGE_VERSION_GIT_SHA short sha or "unknown"
#   PINEFORGE_VERSION_DIRTY   ON/OFF

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

    set(_mmp "${_file_mmp}")
    if(_git_full MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(_mmp "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    endif()

    string(REPLACE "." ";" _parts "${_mmp}")
    list(GET _parts 0 _maj)
    list(GET _parts 1 _min)
    list(GET _parts 2 _pat)

    if(NOT _git_full)
        set(_git_full "${_mmp}")
    endif()

    set(PINEFORGE_VERSION_MAJOR   "${_maj}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_MINOR   "${_min}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_PATCH   "${_pat}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_MMP     "${_mmp}"      PARENT_SCOPE)
    set(PINEFORGE_VERSION_FULL    "${_git_full}" PARENT_SCOPE)
    set(PINEFORGE_VERSION_GIT_SHA "${_git_sha}"  PARENT_SCOPE)
    set(PINEFORGE_VERSION_DIRTY   "${_dirty}"    PARENT_SCOPE)
endfunction()
