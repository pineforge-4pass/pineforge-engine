if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_waiting_whole_bracket_reissue)
    pineforge_add_unit_test(test_waiting_whole_bracket_reissue)
endif()
