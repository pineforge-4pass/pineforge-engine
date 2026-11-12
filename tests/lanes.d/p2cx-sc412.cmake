# Real executable-local syscall interposition; no linker-option fence.
# One registered test per program: with no argument test_p2cx_sc412 runs every row
# (write, invalidate, eof, bypass, budget, default, append, fold-append, fsize,
# thrash, alloc-failure, repeated lookups); a row name as the first argument reruns one row.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_p2cx_sc412)
    pineforge_add_unit_test(test_p2cx_sc412)
    set_tests_properties(test_p2cx_sc412 PROPERTIES LABELS "sc412")
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        list(APPEND TEST_SOURCES test_p2cx_sc412_source)
        pineforge_add_unit_test(test_p2cx_sc412_source)
        set_tests_properties(test_p2cx_sc412_source PROPERTIES LABELS "sc412")
    endif()
endif()
