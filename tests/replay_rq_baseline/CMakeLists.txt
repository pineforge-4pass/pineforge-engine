# Test-only subproject: build the SAME fixture against the old header closure
# and archive. It contains no replay declarations and links no candidate code.
cmake_minimum_required(VERSION 3.16)
project(replay_rq_baseline LANGUAGES CXX)
set(PINEFORGE_REPLAY_BASELINE_SOURCE "" CACHE PATH "Clean pre-change engine source")
set(PINEFORGE_REPLAY_BASELINE_BUILD "" CACHE PATH "Complete pre-change engine build")
if(NOT EXISTS "${PINEFORGE_REPLAY_BASELINE_SOURCE}/include/pineforge/engine.hpp"
   OR NOT EXISTS "${PINEFORGE_REPLAY_BASELINE_BUILD}/lib/libpineforge.a")
    message(FATAL_ERROR "C4 requires a real pre-change header closure and built archive")
endif()
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
add_library(replay_rq_strategy MODULE ../test_replay_rq_fixture.cpp)
target_compile_features(replay_rq_strategy PRIVATE cxx_std_17)
target_compile_definitions(replay_rq_strategy PRIVATE PINEFORGE_REPLAY_RQ_MODULE=1)
target_compile_options(replay_rq_strategy PRIVATE -ffp-contract=off -UNDEBUG)
target_include_directories(replay_rq_strategy PRIVATE
    "${PINEFORGE_REPLAY_BASELINE_SOURCE}/include"
    "${PINEFORGE_REPLAY_BASELINE_BUILD}/include"
    "${PINEFORGE_REPLAY_BASELINE_SOURCE}/src")
target_link_libraries(replay_rq_strategy PRIVATE Eigen3::Eigen)
if(APPLE)
    target_link_options(replay_rq_strategy PRIVATE
        "-Wl,-force_load,${PINEFORGE_REPLAY_BASELINE_BUILD}/lib/libpineforge.a")
else()
    target_link_libraries(replay_rq_strategy PRIVATE
        "-Wl,--whole-archive" "${PINEFORGE_REPLAY_BASELINE_BUILD}/lib/libpineforge.a"
        "-Wl,--no-whole-archive")
endif()
set_target_properties(replay_rq_strategy PROPERTIES
    PREFIX "" SUFFIX ".so" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
