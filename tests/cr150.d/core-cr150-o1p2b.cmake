# CR-150 A4 family 2b (lane core-cr150-o1p2b): the open-entry fee ledger (P22)
# and the ordered membership sets / named-cancel dictionary (P23-P25), with
# their fixed resident bounds and rollback witnesses. P18/P19/P20/P21 of this
# lane's row group are not registered here: see wt/REPORT.md for their
# blocking contract.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_o1p2b_opening_fees)
    pineforge_add_unit_test(test_o1p2b_opening_fees)
    list(APPEND TEST_SOURCES test_o1p2b_a4_sets_tokens)
    pineforge_add_unit_test(test_o1p2b_a4_sets_tokens)
endif()
