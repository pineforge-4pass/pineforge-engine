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
# their example_* rows; the other profiles leave the option OFF.

set(PINEFORGE_NATIVE_EXAMPLES
    hello_kernel
    native_market_strategy
    native_selected_strategy
    native_bracket_strategy
    native_sized_report_strategy
)

# The same host written against the C API (R5 lane L13). It is a separate
# list because it is C, not C++: the C compiler builds it, so any C++ syntax
# leaking into <pineforge/native_c_api.h> breaks this example too, and it
# links with CXX for the runtime's own standard-library symbols.
set(PINEFORGE_NATIVE_C_EXAMPLES
    hello_kernel_c
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
    # Every example is its own assertion: a nonzero exit means the run did not
    # reach Completed, or produced no closed trade.
    foreach(example IN LISTS PINEFORGE_NATIVE_EXAMPLES PINEFORGE_NATIVE_C_EXAMPLES)
        add_test(NAME example_${example} COMMAND example_${example})
        set_tests_properties(example_${example} PROPERTIES TIMEOUT 60)
    endforeach()
    # The regex is the row's assertion (CTest ignores the exit code once one is
    # set), so each of these hosts prints its "closed trades:" line LAST, after
    # every check of its own has passed, and prints nothing of the kind when
    # a check fails.
    set_tests_properties(example_hello_kernel example_hello_kernel_c
        example_native_bracket_strategy
        example_native_sized_report_strategy PROPERTIES
        PASS_REGULAR_EXPRESSION "closed trades: [1-9]")
endif()
