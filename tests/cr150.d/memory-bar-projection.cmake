# Native/feed populations and source matrix/replay storage checks.
# These are ordinary unit rows, not receipt-gated or disabled fixtures.
list(APPEND TEST_SOURCES test_memory_bar_projection)
pineforge_add_unit_test(test_memory_bar_projection)
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_memory_matrix_report_replay)
    pineforge_add_unit_test(test_memory_matrix_report_replay)
endif()
