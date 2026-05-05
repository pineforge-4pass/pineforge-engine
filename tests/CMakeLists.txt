set(TEST_SOURCES
    test_timeframe
    test_magnifier
    test_path_at
    test_na_runtime
    test_recompute
    test_str_utils
    test_ta
    test_kc
    test_matrix
    test_session_time
    test_dmi_parity
    test_integration
    test_request_security
)

foreach(test_name ${TEST_SOURCES})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE pineforge)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Pure-C sanity test for <pineforge/pineforge.h>. Compiled with the C
# compiler so any C++-only syntax leaking into the public header
# triggers a hard build failure. Linked with CXX so it can pull in
# libstdc++ / libc++ symbols transitively through libpineforge.a.
add_executable(test_c_abi test_c_abi.c)
set_target_properties(test_c_abi PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(test_c_abi PRIVATE pineforge)
add_test(NAME test_c_abi COMMAND test_c_abi)
