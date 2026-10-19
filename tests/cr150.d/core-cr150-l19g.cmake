# CR-150 L19G: lifecycle retention and the closed-origin margin reader.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_k1_closed_origin_margin_revival)
    pineforge_add_unit_test(test_k1_closed_origin_margin_revival)
endif()
