# Lane s4e-px2-pendingbook, CR-307: pending projection reads stay linear in
# the live and staged working population while preserving the independent
# projection oracle.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_s4e_px_pendingbook_reads)
    pineforge_add_unit_test(test_s4e_px_pendingbook_reads)
endif()
