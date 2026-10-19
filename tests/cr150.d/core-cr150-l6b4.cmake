# CR-150 L6b-4: independent array oracle for binary scratch writes/pread windows.
if(UNIX)
    list(APPEND TEST_SOURCES test_report_scratch)
    pineforge_add_unit_test(test_report_scratch)
endif()
