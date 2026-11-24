if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_pyramiding_bracket_admission)
    pineforge_add_unit_test(test_pyramiding_bracket_admission)
endif()
