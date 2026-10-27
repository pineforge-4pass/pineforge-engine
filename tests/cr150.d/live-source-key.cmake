# Normal-library public-run controls; no narrow-key adapter here.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_live_source_key_identity)
    pineforge_add_unit_test(test_live_source_key_identity)
    list(APPEND TEST_SOURCES test_live_source_key_spellings)
    pineforge_add_unit_test(test_live_source_key_spellings)
    list(APPEND TEST_SOURCES test_live_source_key_unverified)
    pineforge_add_unit_test(test_live_source_key_unverified)
endif()

# Custom target/row definitions are in the matching main-file fence.
if(PINEFORGE_BUILD_SOURCE_LAYER AND _pf_source_key_type STREQUAL "STATIC_LIBRARY")
    list(APPEND TEST_SOURCES test_live_source_key_alias)
    list(APPEND TEST_SOURCES test_live_source_key_spellings_strict)
    # The matching fence creates the strict target before generation resolves these paths.
    add_test(NAME test_live_source_key_archive
        COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/check_live_source_key_archive.py"
            --config "$<CONFIG>"
            --archive "$<TARGET_FILE:pineforge>"
            --strict-executable "$<TARGET_FILE:test_live_source_key_spellings_strict>")
    set_tests_properties(test_live_source_key_archive PROPERTIES SKIP_RETURN_CODE 77)
endif()
