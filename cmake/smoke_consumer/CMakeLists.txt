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

add_executable(smoke_version main.c)
target_link_libraries(smoke_version PRIVATE PineForge::pineforge)
