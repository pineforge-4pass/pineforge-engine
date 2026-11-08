list(APPEND TEST_SOURCES test_trade_text_source)
pineforge_add_unit_test(test_trade_text_source)
list(APPEND TEST_SOURCES test_trade_text_companion)
pineforge_add_unit_test(test_trade_text_companion)
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_trade_text_strategy)
    pineforge_add_unit_test(test_trade_text_strategy)
    # The report sink writes PINEFORGE_VERSION_STRING (MAJOR.MINOR.PATCH) as its
    # engine field; PINEFORGE_VERSION_FULL gains a describe suffix on a tagged
    # checkout and an -rc.N suffix on a release candidate, so it is not that value.
    add_test(NAME test_trade_text_default_bytes COMMAND ${Python3_EXECUTABLE} -B
        ${PROJECT_SOURCE_DIR}/tests/native_live_trade_text_e2e.py --default-bytes
        $<TARGET_FILE:test_trade_text_strategy> ${PINEFORGE_VERSION_MMP})
endif()
