# Scratch matching fixture (1.7.0 review: the order matcher and the spilled live book).
# Every row of the lane is registered here, so tests/CMakeLists.txt keeps its
# pinned lines. A twin row is its own executable (tests/<row>.cpp, the base
# body with the row's flag fixed), because a lane file names every row after a
# source of its own. All rows but the Pine-host twin are source-free, so they
# also run in the kernel-only profile.
#
# B1-01: the matcher over a book past its resident bound reads no spilled row
# through a saved pointer (the twin _spilled_only runs the spilled book alone:
# the sanitizers profile's AddressSanitizer row). B1-04: a failed spilled read
# inside the noexcept continuation hash ends the run with scratch_io_failed
# instead of std::terminate. B1-05: a winner's lookup walks no queue steps
# however large the book.
list(APPEND TEST_SOURCES test_native_match_spilled_book_snapshot)
pineforge_add_unit_test(test_native_match_spilled_book_snapshot)
list(APPEND TEST_SOURCES test_native_match_spilled_book_snapshot_spilled_only)
pineforge_add_unit_test(test_native_match_spilled_book_snapshot_spilled_only)
list(APPEND TEST_SOURCES test_continuation_hash_spilled_read_failure)
pineforge_add_unit_test(test_continuation_hash_spilled_read_failure)
list(APPEND TEST_SOURCES test_native_match_winner_cost_flat)
pineforge_add_unit_test(test_native_match_winner_cost_flat)
# B1-02: a close bound to external openings keeps them when its book row
# spills and when its accepted event is paged. Core only.
list(APPEND TEST_SOURCES test_core_external_openings_survive_spill)
pineforge_add_unit_test(test_core_external_openings_survive_spill)
# B1-03 (retirement half): a journal retirement whose codec erase fails leaves
# the window and retired_through as they were. Core only.
list(APPEND TEST_SOURCES test_core_codec_retire_failure_is_atomic)
pineforge_add_unit_test(test_core_codec_retire_failure_is_atomic)
# Round 3 (argon F-01..F-04, B1-03 option d): a spilled NetOfSiblings close is
# held across its sibling walk (F-01; twin _spilled_only, the AddressSanitizer
# row); visit_group_recipients copies the applied event before a visitor reads
# the paged journal (F-02); with a journal codec bound the plan-form execution
# is refused, typed (B1-03 (d)).
list(APPEND TEST_SOURCES test_native_match_spilled_net_of_siblings_close)
pineforge_add_unit_test(test_native_match_spilled_net_of_siblings_close)
list(APPEND TEST_SOURCES test_native_match_spilled_net_of_siblings_close_spilled_only)
pineforge_add_unit_test(test_native_match_spilled_net_of_siblings_close_spilled_only)
list(APPEND TEST_SOURCES test_core_visit_group_recipients_visitor_reads_paged_journal)
pineforge_add_unit_test(test_core_visit_group_recipients_visitor_reads_paged_journal)
list(APPEND TEST_SOURCES test_core_codec_plan_execution_refused)
pineforge_add_unit_test(test_core_codec_plan_execution_refused)
# F-03 (rounds 3 to 5): a failed spilled read is never answered as the hash it
# cut short by any reader of the fold: broker_state_hash() (the base row), a
# host that catches its throw (_swallowed), the base engine's projection
# (_base_projection), the two C entry points after the run (_c_entry), and the
# Pine host's projection, continuation capture and scheduler row (_pine_host,
# which includes the source layer and so is built only with it).
list(APPEND TEST_SOURCES test_broker_state_hash_spilled_read_failure)
pineforge_add_unit_test(test_broker_state_hash_spilled_read_failure)
list(APPEND TEST_SOURCES test_broker_state_hash_spilled_read_failure_swallowed)
pineforge_add_unit_test(test_broker_state_hash_spilled_read_failure_swallowed)
list(APPEND TEST_SOURCES test_broker_state_hash_spilled_read_failure_base_projection)
pineforge_add_unit_test(test_broker_state_hash_spilled_read_failure_base_projection)
list(APPEND TEST_SOURCES test_broker_state_hash_spilled_read_failure_c_entry)
pineforge_add_unit_test(test_broker_state_hash_spilled_read_failure_c_entry)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_broker_state_hash_spilled_read_failure_pine_host)
    pineforge_add_unit_test(test_broker_state_hash_spilled_read_failure_pine_host)
endif()
# F-04 and N-01 (rounds 3 and 4): a refused request's external openings are
# copied when its event is built and reload from a page as stored, so no page
# spill reads the caller's call-local cursor after the call (the base row
# frees it: the sanitizers profile's AddressSanitizer control; the twin
# _poisoned keeps it but unreadable, red on any build).
list(APPEND TEST_SOURCES test_core_rejected_external_openings_page_reload)
pineforge_add_unit_test(test_core_rejected_external_openings_page_reload)
list(APPEND TEST_SOURCES test_core_rejected_event_cursor_not_retained)
pineforge_add_unit_test(test_core_rejected_event_cursor_not_retained)
list(APPEND TEST_SOURCES test_core_rejected_event_cursor_not_retained_poisoned)
pineforge_add_unit_test(test_core_rejected_event_cursor_not_retained_poisoned)
