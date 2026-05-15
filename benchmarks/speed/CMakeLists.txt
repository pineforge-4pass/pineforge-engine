# benchmarks/speed/CMakeLists.txt
#
# Google Benchmark target for per-strategy PineForge timing.
# Enabled via: cmake -B build -DPINEFORGE_BUILD_SPEED_BENCH=ON
#
# Requires the pineforge static library to already be defined in the parent
# CMake project (it is — benchmarks/speed is add_subdirectory'd from the
# root CMakeLists.txt only when the option is ON).

cmake_minimum_required(VERSION 3.20)

include(FetchContent)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.0
)
set(BENCHMARK_ENABLE_TESTING    OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(pineforge_bench pineforge_bench.cpp)

target_link_libraries(pineforge_bench PRIVATE pineforge benchmark::benchmark)

target_include_directories(pineforge_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include   # generated version.h
)

target_compile_features(pineforge_bench PRIVATE cxx_std_17)

# Pass the bench-built strategy dylib directory and canonical OHLCV path as
# compile-time string macros so the binary is self-contained (no argv needed).
#
# NOTE: BENCH_STRATEGIES_DIR points at _workdir/build_strategies/ (bench-built
# dylibs from compile_bench_strategy.py), NOT benchmarks/assets/strategies/
# (corpus probes compiled for validation). The bench dylibs are the targets
# for speed measurement.
target_compile_definitions(pineforge_bench PRIVATE
    BENCH_STRATEGIES_DIR="${CMAKE_SOURCE_DIR}/benchmarks/_workdir/build_strategies"
    BENCH_OHLCV_PATH="${CMAKE_SOURCE_DIR}/benchmarks/assets/data/ETHUSDT_15.csv"
)
