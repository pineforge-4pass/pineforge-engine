# A strategy.exit bracket that names no quantity, placed after a
# strategy.entry reversal that reuses the entry id and a later entry of the
# same id: it reserves both lots. Both reversal directions, each beside a
# distinct-id control.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_reused_id_bracket_basis)
    pineforge_add_unit_test(test_reversal_reused_id_bracket_basis)
endif()
