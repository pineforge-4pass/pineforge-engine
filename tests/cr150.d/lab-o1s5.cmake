# Calendar-owner C01/C02 witnesses; both use the UNIX allocation replacement.
if(UNIX)
    list(APPEND TEST_SOURCES test_o1s5_prepared_days)
    list(APPEND TEST_SOURCES test_o1s5_prepared_certificates)
    pineforge_add_unit_test(test_o1s5_prepared_days)
    pineforge_add_unit_test(test_o1s5_prepared_certificates)
endif()
