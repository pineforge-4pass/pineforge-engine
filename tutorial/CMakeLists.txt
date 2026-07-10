# tutorial/CMakeLists.txt — build the tutorial strategy .so files.
#
# Mirrors the per-strategy build in corpus/CMakeLists.txt but kept
# small (one helper, no glob) so the wiring is easy to read alongside
# the tutorial itself.
#
# Outputs:
#   tutorial/macd/strategy.so           (loaded by tutorial/run.py)
#   tutorial/mtf/strategy_htf.so        (loaded by tutorial/run_mtf.py)
#   tutorial/mtf/strategy_ltf.so        (loaded by tutorial/run_mtf.py)
# Targets:
#   strategy_tutorial_macd              (built by tutorial/run.sh)
#   strategy_tutorial_mtf_htf           (built by tutorial/run_mtf.py via cmake)
#   strategy_tutorial_mtf_ltf           (built by tutorial/run_mtf.py via cmake)

# Helper — declares one strategy.so target with all the link-options /
# warning-suppressions / output-path boilerplate the tutorial wants.
function(pineforge_tutorial_strategy target src out_dir out_name)
    add_library(${target} SHARED ${src})
    target_link_libraries(${target} PRIVATE pineforge)

    # Force-load libpineforge so the c_abi.cpp symbols (pf_version_get,
    # strategy_set_trace_enabled) — which the strategy body never
    # references but the Python harnesses expect via dlsym — are pulled in.
    if(APPLE)
        target_link_options(${target} PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    elseif(UNIX)
        target_link_options(${target} PRIVATE
            "-Wl,--whole-archive"
            "$<TARGET_FILE:pineforge>"
            "-Wl,--no-whole-archive")
    endif()

    # Suppress a few warnings the strategy translation unit cannot fix.
    target_compile_options(${target} PRIVATE
        -Wno-unused-but-set-variable
        -Wno-unused-variable
        -Wno-unused-parameter
        -Wno-sign-compare)

    # Place the .so next to its source so the Python harnesses find it
    # at their hard-coded path.
    set_target_properties(${target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${out_dir}
        RUNTIME_OUTPUT_DIRECTORY ${out_dir}
        OUTPUT_NAME ${out_name}
        PREFIX ""
        SUFFIX ".so")
endfunction()

set(TUT_MACD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/macd)
pineforge_tutorial_strategy(strategy_tutorial_macd
    ${TUT_MACD_DIR}/generated.cpp
    ${TUT_MACD_DIR}
    strategy)
message(STATUS "tutorial: configured strategy_tutorial_macd → tutorial/macd/strategy.so")

set(TUT_MTF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mtf)
pineforge_tutorial_strategy(strategy_tutorial_mtf_htf
    ${TUT_MTF_DIR}/generated_htf.cpp
    ${TUT_MTF_DIR}
    strategy_htf)
pineforge_tutorial_strategy(strategy_tutorial_mtf_ltf
    ${TUT_MTF_DIR}/generated_ltf.cpp
    ${TUT_MTF_DIR}
    strategy_ltf)
message(STATUS "tutorial: configured strategy_tutorial_mtf_htf → tutorial/mtf/strategy_htf.so")
message(STATUS "tutorial: configured strategy_tutorial_mtf_ltf → tutorial/mtf/strategy_ltf.so")

# Keep the public streaming tutorial executable in the normal CTest lane so
# its ctypes mirror and lifecycle calls cannot silently drift from the ABI.
if(PINEFORGE_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_tutorial_stream
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_stream.py)
        set_tests_properties(test_tutorial_stream PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        if(PINEFORGE_ENABLE_SANITIZERS AND UNIX)
            # The strategy DSO is ASan-instrumented, but the host Python
            # interpreter is not. Preload the compiler's ASan runtime so its
            # interceptors initialize before ctypes loads the strategy.
            if(APPLE)
                set(_pf_asan_runtime_name
                    "libclang_rt.asan_osx_dynamic.dylib")
                set(_pf_asan_preload_var "DYLD_INSERT_LIBRARIES")
            else()
                set(_pf_asan_runtime_name "libasan.so")
                set(_pf_asan_preload_var "LD_PRELOAD")
            endif()
            execute_process(
                COMMAND ${CMAKE_CXX_COMPILER}
                        -print-file-name=${_pf_asan_runtime_name}
                OUTPUT_VARIABLE _pf_asan_runtime
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(IS_ABSOLUTE "${_pf_asan_runtime}"
                    AND EXISTS "${_pf_asan_runtime}")
                set_tests_properties(test_tutorial_stream PROPERTIES
                    ENVIRONMENT
                        "${_pf_asan_preload_var}=${_pf_asan_runtime}")
            else()
                message(WARNING
                    "ASan runtime not found for test_tutorial_stream: "
                    "${_pf_asan_runtime}")
            endif()
        endif()
    endif()
endif()
