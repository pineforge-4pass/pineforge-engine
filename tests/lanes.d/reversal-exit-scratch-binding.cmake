# The real corpus module and E2E row are registered in the matching fence.
# The run-owner regression also drives a bare native host without project().
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_run_setup_exit_scratch_binding)
    pineforge_add_unit_test(test_run_setup_exit_scratch_binding)
endif()
