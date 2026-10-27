# Linux --wrap accounting; source host fixtures need the source-layer build.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_cr257_source_exit_scratch_io)
    pineforge_add_unit_test(test_cr257_source_exit_scratch_io)
    add_test(NAME test_cr257_source_exit_scratch_io_100k COMMAND test_cr257_source_exit_scratch_io source-100k)
    add_test(NAME test_cr257_scratch_file_io COMMAND test_cr257_source_exit_scratch_io dontneed)
    # The production scope erase batching implemented in 1.7.0:
    # at most ceil(records / 64) teardown transactions for this single scope.
    add_test(NAME test_cr257_exit_scope_io COMMAND test_cr257_source_exit_scratch_io erase-scope)
    set_tests_properties(test_cr257_source_exit_scratch_io PROPERTIES LABELS "cr257")
    set_tests_properties(test_cr257_source_exit_scratch_io_100k PROPERTIES LABELS "cr257;slow")
    set_tests_properties(test_cr257_scratch_file_io PROPERTIES LABELS "cr257")
    set_tests_properties(test_cr257_exit_scope_io PROPERTIES LABELS "cr257;cr265")
endif()
