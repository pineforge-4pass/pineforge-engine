cmake_minimum_required(VERSION 3.16)
project(pineforge_smoke_consumer LANGUAGES C CXX)

find_package(PineForge REQUIRED)

# The package carries libpineforge's no-contraction rule to its consumers
# (the root CMakeLists.txt: INTERFACE -ffp-contract=off): a strategy TU built
# against an export that lost it fuses multiply-adds the library never fuses.
foreach(_pf_target PineForge::pineforge PineForge::kernel)
    get_target_property(_pf_options ${_pf_target} INTERFACE_COMPILE_OPTIONS)
    if(NOT "-ffp-contract=off" IN_LIST _pf_options)
        message(FATAL_ERROR
            "${_pf_target} does not carry -ffp-contract=off to its consumers "
            "(INTERFACE_COMPILE_OPTIONS: ${_pf_options})")
    endif()
endforeach()

# The package's full descriptor keeps a release candidate's -rc.N, which
# find_package's PineForge_VERSION (MAJOR.MINOR.PATCH) cannot carry; it must
# begin with that MAJOR.MINOR.PATCH, and main.c holds it equal to the installed
# header's and the linked library's.
if(NOT DEFINED PineForge_VERSION_FULL)
    message(FATAL_ERROR "PineForgeConfig.cmake does not set PineForge_VERSION_FULL")
endif()
string(LENGTH "${PineForge_VERSION}" _pf_mmp_length)
string(SUBSTRING "${PineForge_VERSION_FULL}" 0 ${_pf_mmp_length} _pf_full_head)
if(NOT _pf_full_head STREQUAL "${PineForge_VERSION}")
    message(FATAL_ERROR "PineForge_VERSION_FULL (${PineForge_VERSION_FULL}) does not "
                        "begin with PineForge_VERSION (${PineForge_VERSION})")
endif()

add_executable(smoke_version main.c)
target_link_libraries(smoke_version PRIVATE PineForge::pineforge)
target_compile_definitions(smoke_version PRIVATE
    "PF_SMOKE_PACKAGE_VERSION_FULL=\"${PineForge_VERSION_FULL}\"")
