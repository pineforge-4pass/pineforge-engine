# CR-150 O1W lane o1w-tradeout: R12 terminal report rows and R13 summary
# trades. Plain registrations only; nothing here reconfigures another lane.
list(APPEND TEST_SOURCES test_open_report_rows_flat)
pineforge_add_unit_test(test_open_report_rows_flat)
set_tests_properties(test_open_report_rows_flat PROPERTIES LABELS "slow;mem")
# The API-free RED witness: it compiles on the pre-lane tree and fails there
# because the terminal rows are retained per open lot.
list(APPEND TEST_SOURCES test_open_report_rows_retention_flat)
pineforge_add_unit_test(test_open_report_rows_retention_flat)
set_tests_properties(test_open_report_rows_retention_flat PROPERTIES LABELS "slow;mem")
list(APPEND TEST_SOURCES test_report_trades_summary_flat)
pineforge_add_unit_test(test_report_trades_summary_flat)
set_tests_properties(test_report_trades_summary_flat PROPERTIES LABELS "slow;mem")
