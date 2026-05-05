cmake_minimum_required(VERSION 3.16)
project(PineForge
    VERSION 0.1.0
    DESCRIPTION "Deterministic PineScript v6 backtest runtime, validated trade-for-trade against TradingView"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Build artefact layout
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(PINEFORGE_BUILD_TESTS "Build the C++ test suite" ON)
option(PINEFORGE_BUILD_EXAMPLES "Build example consumers (none yet)" OFF)
option(PINEFORGE_BUILD_CORPUS_STRATEGIES
    "Build a strategy.so per .pine in corpus/ from its generated.cpp" OFF)

# === Eigen3 ============================================================
# Used for matrix-typed PineScript (matrix.* API). Tries system install
# first; falls back to FetchContent.
find_package(Eigen3 3.3 QUIET)
if(NOT Eigen3_FOUND)
    message(STATUS "System Eigen not found, fetching via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(
        eigen
        URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    )
    set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(eigen)
else()
    message(STATUS "Found system Eigen: ${Eigen3_DIR}")
endif()

# === pineforge library =================================================
add_library(pineforge STATIC
    src/c_abi.cpp
    src/engine_fills.cpp
    src/engine_lower_tf.cpp
    src/engine_orders.cpp
    src/engine_path_resolve.cpp
    src/engine_report.cpp
    src/engine_risk.cpp
    src/engine_run.cpp
    src/engine_security.cpp
    src/engine_strategy_commands.cpp
    src/engine_trade_accessors.cpp
    src/magnifier.cpp
    src/math.cpp
    src/matrix.cpp
    src/session_time.cpp
    src/str_utils.cpp
    src/ta_extremes_volume.cpp
    src/ta_misc.cpp
    src/ta_moving_averages.cpp
    src/ta_oscillators.cpp
    src/ta_volatility_trend.cpp
    src/timeframe.cpp
    src/timezone.cpp
)

# Canonical alias for downstream find_package(PineForge) consumers.
add_library(PineForge::pineforge ALIAS pineforge)

# Hidden visibility by default — only symbols explicitly tagged with
# PF_API in <pineforge/pineforge.h> are exported in any .so that
# statically links libpineforge.a. Internal C++ classes (BacktestEngine,
# ta::*, internal::*) remain hidden post-link.
target_compile_options(pineforge PRIVATE
    -O2
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

target_include_directories(pineforge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pineforge PUBLIC Eigen3::Eigen)

# === Tests =============================================================
if(PINEFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# === Corpus strategies (opt-in) ========================================
if(PINEFORGE_BUILD_CORPUS_STRATEGIES)
    add_subdirectory(corpus)
endif()

# === Install / package config =========================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS pineforge
    EXPORT PineForgeTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/pineforge
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

install(EXPORT PineForgeTargets
    FILE PineForgeTargets.cmake
    NAMESPACE PineForge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PineForge
)

configure_package_config_file(
    cmake/PineForgeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PineForgeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PineForge
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PineForgeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PineForgeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PineForgeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PineForge
)

# === Summary ===========================================================
message(STATUS "")
message(STATUS "PineForge ${PROJECT_VERSION}")
message(STATUS "  build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "  install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  build tests:       ${PINEFORGE_BUILD_TESTS}")
message(STATUS "  build corpus:      ${PINEFORGE_BUILD_CORPUS_STRATEGIES}")
message(STATUS "")
