# A journal read that fails inside the search native_events makes reaches the
# caller as the typed scratch failure the journal's store raised; it does not
# end the process. Source-free, so the row also runs in the kernel-only
# profile. POSIX only: each case runs in a forked child, and the fault is a
# truncated scratch descriptor.
if(UNIX)
    list(APPEND TEST_SOURCES test_native_events_journal_failure)
    pineforge_add_unit_test(test_native_events_journal_failure)
endif()
