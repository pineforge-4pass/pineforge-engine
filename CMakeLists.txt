cmake_minimum_required(VERSION 3.16)

# Resolve version from git tag (preferred) or VERSION file (fallback).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PineForgeVersion)
pineforge_resolve_version()

project(PineForge
    VERSION ${PINEFORGE_VERSION_MMP}
    DESCRIPTION "Deterministic PineScript v6 backtest runtime, validated trade-for-trade against TradingView"
    LANGUAGES C CXX
)

# Generate <pineforge/version.h> from version.h.in. Lives in the build
# tree; installed alongside the curated headers below.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pineforge/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/pineforge/version.h"
    @ONLY
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
option(PINEFORGE_BUILD_TUTORIAL
    "Build the tutorial MACD strategy.so under tutorial/" ON)
option(PINEFORGE_ENABLE_COVERAGE
    "Instrument the runtime + tests for source-level coverage (Clang or GCC)" OFF)
# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer. Off by default
# (slows the runtime and is incompatible with bit-exact perf builds).
# Build with:
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPINEFORGE_ENABLE_SANITIZERS=ON
option(PINEFORGE_ENABLE_SANITIZERS
    "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

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
    # Optimization level only for non-Debug configs — baking -O2 here
    # unconditionally would defeat -DCMAKE_BUILD_TYPE=Debug (no breakpoints
    # / inlined-away locals). The FP flags below stay unconditional because
    # parity depends on them in every build type.
    $<$<NOT:$<CONFIG:Debug>>:-O2>
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Disable floating-point contraction (FMA generation). TradingView's
    # Pine runtime executes in JavaScript, which has no FMA primitive —
    # every multiply-add is two separately-rounded operations. With the
    # default -O2 + clang behaviour (`-ffp-contract=on` for in-statement
    # contraction) the engine emits FMA on ARM64 / AVX2, producing
    # results 1-2 ULP closer to the true value than Pine. That sounds
    # like a win but it's the wrong target — we want bit-exact parity
    # with TradingView, not "more accurate". The 1-2 ULP drift compounds
    # across hundreds of RMA iterations and can flip RSI threshold
    # crossings on close-call bars (see tests/test_ta_rma_warmup.cpp).
    # `-ffp-contract=off` forces the compiler to round each * and + to
    # IEEE-754 double independently, matching the Pine reference.
    -ffp-contract=off
)

target_include_directories(pineforge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(pineforge PUBLIC Eigen3::Eigen)

# === Coverage instrumentation (opt-in) =================================
# Build with:
#   cmake -B build -DCMAKE_BUILD_TYPE=Debug -DPINEFORGE_ENABLE_COVERAGE=ON
# then drive the report via `bash scripts/coverage.sh`. Clang emits
# llvm-profile (.profraw) which `llvm-cov` consumes; GCC emits .gcda /
# .gcno which `gcov` / `gcovr` / `lcov` consume.
if(PINEFORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_pf_cov_flags -fprofile-instr-generate -fcoverage-mapping)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(_pf_cov_flags --coverage -fprofile-arcs -ftest-coverage)
    else()
        message(FATAL_ERROR
            "PINEFORGE_ENABLE_COVERAGE requires Clang or GCC (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    target_compile_options(pineforge PRIVATE ${_pf_cov_flags})
    target_link_options(pineforge PUBLIC ${_pf_cov_flags})
    message(STATUS "Coverage instrumentation: ${_pf_cov_flags}")
endif()

# === Sanitizers (opt-in) ==============================================
# ASan + UBSan. Link options are PUBLIC so anything linking libpineforge.a
# (tests, corpus strategy.so) picks up the runtime.
if(PINEFORGE_ENABLE_SANITIZERS)
    target_compile_options(pineforge PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pineforge PUBLIC -fsanitize=address,undefined)
    message(STATUS "Sanitizers: -fsanitize=address,undefined")
endif()

# === Tests =============================================================
if(PINEFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# === Tutorial strategy (opt-out) =======================================
if(PINEFORGE_BUILD_TUTORIAL)
    add_subdirectory(tutorial)
endif()

# === Corpus strategies (opt-in) ========================================
if(PINEFORGE_BUILD_CORPUS_STRATEGIES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/CMakeLists.txt")
        add_subdirectory(corpus)
    else()
        message(FATAL_ERROR
            "PINEFORGE_BUILD_CORPUS_STRATEGIES=ON but corpus/CMakeLists.txt is missing. "
            "Init the private validation submodule (maintainers): "
            "  git submodule update --init corpus\n"
            "See CONTRIBUTING.md.")
    endif()
endif()

# === Bench strategies (opt-in) ========================================
# Builds one strategy.dylib/.so per bench strategy from committed generated.cpp.
# Requires the benchmarks/assets submodule to be initialised:
#   git submodule update --init benchmarks/assets
# Build with:
#   cmake -B build -DPINEFORGE_BUILD_BENCH_STRATEGIES=ON
#   cmake --build build --target bench_strategies -j
option(PINEFORGE_BUILD_BENCH_STRATEGIES
    "Build benchmarks bench strategies (.dylib/.so per strategy)" OFF)
if(PINEFORGE_BUILD_BENCH_STRATEGIES)
    set(_bench_strat_cmake
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/assets/strategies/CMakeLists.txt")
    if(EXISTS "${_bench_strat_cmake}")
        add_subdirectory(benchmarks/assets/strategies)
    else()
        message(FATAL_ERROR
            "PINEFORGE_BUILD_BENCH_STRATEGIES=ON but "
            "benchmarks/assets/strategies/CMakeLists.txt is missing.\n"
            "Init the assets submodule:\n"
            "  git submodule update --init benchmarks/assets\n"
            "See CONTRIBUTING.md.")
    endif()
endif()

# === Speed bench (opt-in) =============================================
# Google Benchmark target for per-strategy PineForge timing.
# Build with: cmake -B build -DPINEFORGE_BUILD_SPEED_BENCH=ON
# Requires bench-built strategy dylibs alongside generated.cpp in assets/strategies/.
# Build bench strategies first: cmake --build build --target bench_strategies
option(PINEFORGE_BUILD_SPEED_BENCH "Build benchmarks/speed GBench target" OFF)
if(PINEFORGE_BUILD_SPEED_BENCH)
    add_subdirectory(benchmarks/speed)
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

# Generated version header — installed alongside the curated ones.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/include/pineforge/version.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pineforge
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
message(STATUS "PineForge ${PINEFORGE_VERSION_FULL}")
message(STATUS "  build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:      ${CMAKE_CXX_STANDARD}")
message(STATUS "  install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  build tests:       ${PINEFORGE_BUILD_TESTS}")
message(STATUS "  build tutorial:    ${PINEFORGE_BUILD_TUTORIAL}")
message(STATUS "  build corpus:      ${PINEFORGE_BUILD_CORPUS_STRATEGIES}")
message(STATUS "  build bench strat: ${PINEFORGE_BUILD_BENCH_STRATEGIES}")
message(STATUS "")
