# The native command journal's spilled pages on read-back.
# One page of the journal's scratch file is overwritten with zeros after a run
# that spilled, and native_events(after) is asked for a cursor whose answer
# begins inside that page: the read must fail with a coded failure, never
# answer wrong command rows. A control host runs the same batch and the same
# call with nothing zeroed. Source-free, so the row also runs in the
# kernel-only profile. Exit 77 only where the journal's spill has no
# descriptor of its own (a scratch arena is attached).
list(APPEND TEST_SOURCES test_native_journal_zeroed_spill_page)
pineforge_add_unit_test(test_native_journal_zeroed_spill_page)
set_tests_properties(test_native_journal_zeroed_spill_page PROPERTIES SKIP_RETURN_CODE 77)
