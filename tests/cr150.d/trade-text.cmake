list(APPEND TEST_SOURCES test_trade_text_source)
pineforge_add_unit_test(test_trade_text_source)
list(APPEND TEST_SOURCES test_trade_text_companion)
pineforge_add_unit_test(test_trade_text_companion)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_trade_text_strategy)
    pineforge_add_unit_test(test_trade_text_strategy)
    add_test(NAME test_trade_text_default_bytes COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/native_live_trade_text_e2e.py --default-bytes
        $<TARGET_FILE:test_trade_text_strategy> ${PINEFORGE_VERSION_FULL})
endif()
