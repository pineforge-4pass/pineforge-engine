# strategy.close(id) after a strategy.entry reversal that reuses the entry id,
# in the cases tests/test_reversal_reused_id_purge_credit.cpp and
# tests/test_flip_reused_id_closed_row_debit.cpp do not place: a percentage
# and a quantity close after a reversal into short whose closed lot is not the
# smaller one, and a full close under the ANY rule and under the default
# first-in-first-out rule after a reversal in either direction. Each beside a
# distinct-id control.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_reused_id_close_by_id)
    pineforge_add_unit_test(test_reversal_reused_id_close_by_id)
endif()
