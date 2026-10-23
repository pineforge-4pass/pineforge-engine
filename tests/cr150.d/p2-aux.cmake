# P2 auxiliary release findings: sidecar/temp admission, payload parity and
# pre-retirement journal refolding.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_p2_aux_payload_codec)
    pineforge_add_unit_test(test_p2_aux_payload_codec)
    list(APPEND TEST_SOURCES test_p2_aux_market_admission)
    pineforge_add_unit_test(test_p2_aux_market_admission)
endif()
