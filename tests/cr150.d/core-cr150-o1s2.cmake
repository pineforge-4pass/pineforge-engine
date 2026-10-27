# This row is compiled/executed only by the release verification hosts.
if(UNIX)
    list(APPEND TEST_SOURCES test_o1s2_native_readers)
    pineforge_add_unit_test(test_o1s2_native_readers)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        list(APPEND TEST_SOURCES test_o1s2_payload_readers)
        pineforge_add_unit_test(test_o1s2_payload_readers)
    endif()
endif()
