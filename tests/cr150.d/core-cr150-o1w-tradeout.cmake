# CR-150 O1W lane o1w-tradeout: R12 terminal report rows and R13 summary
# trades. Plain registrations only; nothing here reconfigures another lane.
list(APPEND TEST_SOURCES test_open_report_rows_flat)
pineforge_add_unit_test(test_open_report_rows_flat)
# Independent population slices share the executable above.
add_test(NAME test_open_report_rows_flat_slice_1
    COMMAND test_open_report_rows_flat --slice 1/4)
add_test(NAME test_open_report_rows_flat_slice_2
    COMMAND test_open_report_rows_flat --slice 2/4)
add_test(NAME test_open_report_rows_flat_slice_3
    COMMAND test_open_report_rows_flat --slice 3/4)
add_test(NAME test_open_report_rows_flat_slice_4
    COMMAND test_open_report_rows_flat --slice 4/4)
add_test(NAME test_open_report_rows_flat_slices_cover
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test_open_report_rows_flat_split.py)
set_tests_properties(
    test_open_report_rows_flat_slice_1
    test_open_report_rows_flat_slice_2
    test_open_report_rows_flat_slice_3
    test_open_report_rows_flat_slice_4
    PROPERTIES LABELS "slow;mem")
# The API-free RED witness: it compiles on the pre-lane tree and fails there
# because the terminal rows are retained per open lot.
list(APPEND TEST_SOURCES test_open_report_rows_retention_flat)
pineforge_add_unit_test(test_open_report_rows_retention_flat)
set_tests_properties(test_open_report_rows_retention_flat PROPERTIES LABELS "slow;mem")
list(APPEND TEST_SOURCES test_report_trades_summary_flat)
pineforge_add_unit_test(test_report_trades_summary_flat)
set_tests_properties(test_report_trades_summary_flat PROPERTIES LABELS "slow;mem")
list(APPEND TEST_SOURCES test_report_sink_trade_residency)
pineforge_add_unit_test(test_report_sink_trade_residency)
set_tests_properties(test_report_sink_trade_residency PROPERTIES
    LABELS "slow;mem;LAB-DISKSCRATCH" RUN_SERIAL TRUE)
