# Lane s4e-fix-b1, round 7 (argon F-NEW-01): a resident winner the full scan
# holds is read after observe_trails, which can promote a spilled trail into
# the slot it names (the twin _spilled_only runs the spilled book alone: the
# sanitizers profile's row). Source-free, so both rows also run in the
# kernel-only profile.
list(APPEND TEST_SOURCES test_native_match_winner_alias_after_trail_promotion)
pineforge_add_unit_test(test_native_match_winner_alias_after_trail_promotion)
list(APPEND TEST_SOURCES test_native_match_winner_alias_after_trail_promotion_spilled_only)
pineforge_add_unit_test(test_native_match_winner_alias_after_trail_promotion_spilled_only)
