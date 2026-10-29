if(UNIX)
    list(APPEND TEST_SOURCES test_closed_trade_text_reads)
    pineforge_add_unit_test(test_closed_trade_text_reads)
endif()
