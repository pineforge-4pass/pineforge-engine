# The 1.8.0 reversal fix, one row per guard. The purge row reverses into long,
# the only reversal the purge runs for, and checks that the reversing id keeps
# its own opening. The debit row flips by a strategy.order that crosses zero
# and by a reversal into short, and checks that the closed-trade row is not
# taken out of the new opening.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_reused_id_purge_credit)
    pineforge_add_unit_test(test_reversal_reused_id_purge_credit)
    list(APPEND TEST_SOURCES test_flip_reused_id_closed_row_debit)
    pineforge_add_unit_test(test_flip_reused_id_closed_row_debit)
endif()
