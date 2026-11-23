# A strategy.entry reversal and its sign image leave the same book. The first
# row holds the exits a reversal ends to one book in both directions. The
# second row holds a reversal the position-sign counter does not see, and the
# reused-id reversal after it, to the same numbers in both directions.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_direction)
    pineforge_add_unit_test(test_reversal_purge_direction)
    list(APPEND TEST_SOURCES test_batched_reversal_direction)
    pineforge_add_unit_test(test_batched_reversal_direction)
endif()
