# CR-150 L6b-4: independent array oracle for binary scratch writes/pread windows.
if(UNIX)
    list(APPEND TEST_SOURCES test_report_scratch)
    pineforge_add_unit_test(test_report_scratch)
endif()
# CR-150 L6b-4 fix round 1: the engine write side of the scratch curve, against the sink-off array oracle.
if(UNIX)
    list(APPEND TEST_SOURCES test_report_sink_scratch_curve)
    pineforge_add_unit_test(test_report_sink_scratch_curve)
    # Measures the scratch curve on a disk-backed file; fails on the CLOUD overlayfs container.
    set_tests_properties(test_report_sink_scratch_curve PROPERTIES LABELS "LAB-DISKSCRATCH")
endif()
