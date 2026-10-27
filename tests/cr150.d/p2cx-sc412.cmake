# Real executable-local syscall interposition; no linker-option fence.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_p2cx_sc412)
    pineforge_add_unit_test(test_p2cx_sc412)
    add_test(NAME test_p2cx_sc412_write COMMAND test_p2cx_sc412 write)
    add_test(NAME test_p2cx_sc412_invalidate COMMAND test_p2cx_sc412 invalidate)
    add_test(NAME test_p2cx_sc412_eof COMMAND test_p2cx_sc412 eof)
    add_test(NAME test_p2cx_sc412_bypass COMMAND test_p2cx_sc412 bypass)
    add_test(NAME test_p2cx_sc412_budget COMMAND test_p2cx_sc412 budget)
    add_test(NAME test_p2cx_sc412_default COMMAND test_p2cx_sc412 default)
    add_test(NAME test_p2cx_sc412_append COMMAND test_p2cx_sc412 append)
    add_test(NAME test_p2cx_sc412_fold_append COMMAND test_p2cx_sc412 fold_append)
    add_test(NAME test_p2cx_sc412_fsize COMMAND test_p2cx_sc412 fsize)
    add_test(NAME test_p2cx_sc412_thrash COMMAND test_p2cx_sc412 thrash)
    add_test(NAME test_p2cx_sc412_alloc_failure COMMAND test_p2cx_sc412 alloc_failure)
    set_tests_properties(
        test_p2cx_sc412 test_p2cx_sc412_write test_p2cx_sc412_invalidate
        test_p2cx_sc412_eof test_p2cx_sc412_bypass test_p2cx_sc412_default
        test_p2cx_sc412_append test_p2cx_sc412_fold_append test_p2cx_sc412_fsize
        test_p2cx_sc412_thrash test_p2cx_sc412_alloc_failure
        PROPERTIES LABELS "sc412")
    set_tests_properties(test_p2cx_sc412_budget PROPERTIES LABELS "sc412;slow")
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        list(APPEND TEST_SOURCES test_p2cx_sc412_source)
        pineforge_add_unit_test(test_p2cx_sc412_source)
        set_tests_properties(test_p2cx_sc412_source PROPERTIES LABELS "sc412")
    endif()
endif()
