# A separate executable owns every replaceable allocation form. Source
# scenarios use the real Pine host; no private fixture files are required.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_release_run_phases)
    pineforge_add_unit_test(test_memory_release_run_phases)
endif()
