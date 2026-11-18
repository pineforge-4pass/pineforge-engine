# The fixture includes hold private chart data; without them the rows are not registered.
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/fixtures/ta_tie_cells/eurusd_15.inc")
    list(APPEND TEST_SOURCES test_ta_tie_cell_eurusd)
    pineforge_add_unit_test(test_ta_tie_cell_eurusd)
endif()
if(EXISTS "${PROJECT_SOURCE_DIR}/tests/fixtures/ta_tie_cells/xauusd_15.inc")
    list(APPEND TEST_SOURCES test_ta_tie_cell_xauusd)
    pineforge_add_unit_test(test_ta_tie_cell_xauusd)
endif()
