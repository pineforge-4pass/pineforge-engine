if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_reversal_named_bracket_lifetime)
    pineforge_add_unit_test(test_reversal_named_bracket_lifetime)
endif()
