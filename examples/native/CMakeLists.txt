# Top-level, Pine-free native examples. Enabled by -DPINEFORGE_BUILD_EXAMPLES=ON.
#
# Each source is a complete, self-contained C++ host: no SQLite / curl /
# OpenSSL (those belong to the optional live runner), no PineScript, no
# codegen. Two of them double as the loadable MODULE targets the live runner
# dlopens — runner/CMakeLists.txt builds the same sources with
# PINEFORGE_EXPORT_NATIVE_STRATEGY providing the C ABI.
#
# Every example links PineForge::kernel, the archive that holds the kernel
# alone: a source-layer symbol reaching an example is a link error here in the
# default build too, not only under PINEFORGE_BUILD_SOURCE_LAYER=OFF. The
# release and kernel profiles of scripts/ci_verify.py build them and run
# their example_* rows (each asserts the exit code and a summary line, below);
# the other profiles leave the option OFF.

set(PINEFORGE_NATIVE_EXAMPLES
    hello_kernel
    native_market_strategy
    native_selected_strategy
    native_bracket_strategy
    native_sized_report_strategy
    native_margin_strategy
    native_calc_on_fills_strategy
    native_htf_strategy
    native_trail_risk_strategy
    native_price_grid_strategy
    native_risk_limits_strategy
    native_auxiliary_feed_strategy
    native_open_lots_strategy
)

# The same host written against the C API (R5 lane L13). It is a separate
# list because it is C, not C++: the C compiler builds it, so any C++ syntax
# leaking into <pineforge/native_c_api.h> breaks this example too, and it
# links with CXX for the runtime's own standard-library symbols.
set(PINEFORGE_NATIVE_C_EXAMPLES
    hello_kernel_c
    native_price_grid_c
)

foreach(example IN LISTS PINEFORGE_NATIVE_EXAMPLES)
    set(target example_${example})
    add_executable(${target} ${example}.cpp)
    target_link_libraries(${target} PRIVATE PineForge::kernel)
    # Same floating-point contract as the library: parity depends on it.
    # -Wno-unused-parameter matches the library's own option set: the public
    # headers declare virtuals with named, unused parameters.
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter -ffp-contract=off)
    # Each example checks its own results, so keep assert() live in it: strip
    # NDEBUG (appended after the build-type flags, so it wins over Release's
    # -DNDEBUG), as for every test target, or an assert() added to an example
    # compiles to a no-op and its example_* row passes vacuously.
    # scripts/ci_verify.py's examples-assert-live stage checks every example's
    # compile command for it.
    target_compile_options(${target} PRIVATE -UNDEBUG)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${example}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(PINEFORGE_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

foreach(example IN LISTS PINEFORGE_NATIVE_C_EXAMPLES)
    set(target example_${example})
    add_executable(${target} ${example}.c)
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(${target} PRIVATE PineForge::kernel)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter -ffp-contract=off)
    # assert() stays live here too (<assert.h> reads NDEBUG the same way).
    target_compile_options(${target} PRIVATE -UNDEBUG)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${example}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    if(PINEFORGE_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

if(PINEFORGE_BUILD_TESTS)
    # Every example is its own assertion, and its row asserts two things: the
    # host exits 0 (it reached Completed and every check of its own passed),
    # and it printed its summary line, which each host prints only after those
    # checks and never when one fails. CTest cannot assert both on one row --
    # PASS_REGULAR_EXPRESSION replaces the exit-code check, so a host that
    # printed its line and then exited nonzero passed -- so every row runs its
    # host through run_example.cmake, which fails on a nonzero exit, a signal,
    # a timeout or a missing line. The line below is that row's assertion; an
    # example without one is a configure error. No row may set
    # PASS_REGULAR_EXPRESSION, FAIL_REGULAR_EXPRESSION, SKIP_REGULAR_EXPRESSION,
    # WILL_FAIL or SKIP_RETURN_CODE: each would override the runner's verdict.
    set(_pf_example_line_hello_kernel "closed trades: [1-9]")
    set(_pf_example_line_hello_kernel_c "closed trades: [1-9]")
    set(_pf_example_line_native_market_strategy
        "batch closed trades: [1-9][0-9]*  stream closed trades: [1-9]")
    set(_pf_example_line_native_selected_strategy
        "host-sized applied: 1 units=2  selected-close applied: 1  closed trades: [1-9]")
    set(_pf_example_line_native_bracket_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_sized_report_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_margin_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_calc_on_fills_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_htf_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_trail_risk_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_price_grid_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_risk_limits_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_price_grid_c "closed trades: [1-9]")
    set(_pf_example_line_native_auxiliary_feed_strategy "closed trades: [1-9]")
    set(_pf_example_line_native_open_lots_strategy "closed trades: [1-9]")
    foreach(example IN LISTS PINEFORGE_NATIVE_EXAMPLES PINEFORGE_NATIVE_C_EXAMPLES)
        if(NOT DEFINED _pf_example_line_${example})
            message(FATAL_ERROR "examples/native: ${example} has no summary line for its "
                "CTest row to assert; set _pf_example_line_${example} to the line it prints "
                "after its own checks have passed")
        endif()
        add_test(NAME example_${example}
            COMMAND ${CMAKE_COMMAND}
                "-DEXAMPLE=$<TARGET_FILE:example_${example}>"
                "-DEXPECT=${_pf_example_line_${example}}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
        set_tests_properties(example_${example} PROPERTIES TIMEOUT 60)
    endforeach()

    # The runner's mutation self-test: a host that prints its line and then
    # exits nonzero, dies on a signal or hangs fails it, as does one that exits
    # 0 without the line; and every example_* row above, read back from CTest,
    # runs its own host through it with no property overriding its verdict.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME test_example_runner
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/scripts/test_example_runner.py
            --cmake ${CMAKE_COMMAND} --ctest ${CMAKE_CTEST_COMMAND}
            --examples-build-dir ${CMAKE_CURRENT_BINARY_DIR})
endif()
