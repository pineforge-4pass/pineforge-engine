cmake_minimum_required(VERSION 3.16)
# Each numeric producer uses its OWN version module, template and VERSION.
if(NOT DEFINED A5_TREE OR NOT DEFINED A5_GENERATED)
    message(FATAL_ERROR "A5_TREE and A5_GENERATED are required")
endif()
set(CMAKE_CURRENT_SOURCE_DIR "${A5_TREE}")
set(PINEFORGE_VERSION_SOURCE FILE CACHE STRING "" FORCE)
include("${A5_TREE}/cmake/PineForgeVersion.cmake")
pineforge_resolve_version()
configure_file("${A5_TREE}/include/pineforge/version.h.in"
    "${A5_GENERATED}/pineforge/version.h" @ONLY)
