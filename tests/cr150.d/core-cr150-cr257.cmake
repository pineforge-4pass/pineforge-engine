# Linux --wrap accounting; source host fixtures need the source-layer build.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_cr257_source_exit_scratch_io)
    pineforge_add_unit_test(test_cr257_source_exit_scratch_io)
    add_test(NAME test_cr257_source_exit_scratch_io_100k COMMAND test_cr257_source_exit_scratch_io source-100k)
    add_test(NAME test_cr257_scratch_file_io COMMAND test_cr257_source_exit_scratch_io dontneed)
    # test_cr257_exit_scope_io (erase-scope) is the CR-265 layer (c) red test: it
    # is registered by CR-265 (1.7.1), not by the 1.7.0 bundle.
    set_tests_properties(test_cr257_source_exit_scratch_io PROPERTIES LABELS "cr257")
    set_tests_properties(test_cr257_source_exit_scratch_io_100k PROPERTIES LABELS "cr257;slow")
    set_tests_properties(test_cr257_scratch_file_io PROPERTIES LABELS "cr257")
endif()
