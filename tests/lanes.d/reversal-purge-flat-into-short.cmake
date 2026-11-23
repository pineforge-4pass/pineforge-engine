# A strategy.entry reversal whose entry was placed on a flat book and whose
# fill opens short ends the exits of the long side its fill closed, and keeps
# those of the short side it opens. The row holds a bracket bound to a limit
# entry resting on each side to that book, beside a control with no reversal.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_purge_flat_into_short)
    pineforge_add_unit_test(test_reversal_purge_flat_into_short)
endif()
