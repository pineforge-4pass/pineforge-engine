# A strategy.entry reversal sends a cancel only for the exit orders that still
# work when its fill is applied. The row holds the journal of a reversal whose
# bracket the kernel has already ended to no not-working event, into long and
# into short, beside a control whose bracket still works and is cancelled.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_working_only)
    pineforge_add_unit_test(test_reversal_purge_working_only)
endif()
