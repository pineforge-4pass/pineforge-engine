# strategies/rl-qlearn/CMakeLists.txt — build the RL demo strategy .so.
#
# Mirrors the per-strategy wiring in tutorial/CMakeLists.txt (kept local so
# this directory builds even when PINEFORGE_BUILD_TUTORIAL=OFF).
#
# Output:  strategies/rl-qlearn/strategy.so   (loaded by run_rl.py)
# Target:  strategy_rl_qlearn

add_library(strategy_rl_qlearn SHARED strategy.cpp)
target_link_libraries(strategy_rl_qlearn PRIVATE pineforge)

# Force-load libpineforge so the c_abi.cpp symbols the Python harness
# resolves via dlsym (pf_version_get, strategy_get_last_error, ...) are
# pulled into the .so even though strategy.cpp never references them.
if(APPLE)
    target_link_options(strategy_rl_qlearn PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:pineforge>")
elseif(UNIX)
    target_link_options(strategy_rl_qlearn PRIVATE
        "-Wl,--whole-archive"
        "$<TARGET_FILE:pineforge>"
        "-Wl,--no-whole-archive")
endif()

set_target_properties(strategy_rl_qlearn PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_NAME strategy
    PREFIX ""
    SUFFIX ".so")

message(STATUS "strategies: configured strategy_rl_qlearn → strategies/rl-qlearn/strategy.so")
