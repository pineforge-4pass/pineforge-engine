# Public host witness: lifecycle of the held lot's stop before a typed market reversal settles.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_typed_reversal_bracket)
    pineforge_add_unit_test(test_typed_reversal_bracket)
endif()
