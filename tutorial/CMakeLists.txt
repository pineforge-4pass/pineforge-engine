# tutorial/CMakeLists.txt — build the tutorial MACD strategy.so.
#
# Mirrors the per-strategy build in corpus/CMakeLists.txt but kept
# small (one strategy, no glob) so the wiring is easy to read alongside
# the tutorial itself.
#
# Output:  tutorial/macd/strategy.so   (loaded by tutorial/run.py)
# Target:  strategy_tutorial_macd      (built by tutorial/run.sh)

set(TUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/macd)

add_library(strategy_tutorial_macd SHARED ${TUT_DIR}/generated.cpp)

target_link_libraries(strategy_tutorial_macd PRIVATE pineforge)

# Force-load libpineforge so the c_abi.cpp symbols (pf_version_get,
# strategy_set_trace_enabled) — which the codegen-emitted body never
# references but tutorial/run.py expects via dlsym — are pulled in.
if(APPLE)
    target_link_options(strategy_tutorial_macd PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:pineforge>")
elseif(UNIX)
    target_link_options(strategy_tutorial_macd PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:pineforge>"
        "-Wl,--no-whole-archive")
endif()

# Codegen output triggers a few warnings we cannot fix at this layer.
target_compile_options(strategy_tutorial_macd PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-sign-compare)

# Place the .so next to its source (so tutorial/run.py finds it at the
# default path tutorial/macd/strategy.so).
set_target_properties(strategy_tutorial_macd PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${TUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${TUT_DIR}
    OUTPUT_NAME strategy
    PREFIX ""
    SUFFIX ".so")

message(STATUS "tutorial: configured strategy_tutorial_macd → tutorial/macd/strategy.so")
