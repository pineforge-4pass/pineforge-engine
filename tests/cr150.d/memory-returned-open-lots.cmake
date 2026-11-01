# open_lot_snapshot: owning native open-lot snapshots; no source-layer dependency.
list(APPEND TEST_SOURCES test_memory_returned_open_lots)
pineforge_add_unit_test(test_memory_returned_open_lots)
