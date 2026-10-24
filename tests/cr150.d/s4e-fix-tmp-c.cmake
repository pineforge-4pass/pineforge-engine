# Exact 1.6.3 create text; test_successor_roots_ram remains pinned and untouched.
# The new row also reaches that wrapper so m6 has both exact-text witnesses.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_scratch_create_pinned_text)
    pineforge_add_unit_test(test_scratch_create_pinned_text)
    set_tests_properties(test_scratch_create_pinned_text PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
endif()
