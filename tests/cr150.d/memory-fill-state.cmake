# Observe real fill paths and stored state through the test-side seam.
if(UNIX)
    list(APPEND TEST_SOURCES test_memory_native_state)
    pineforge_add_unit_test(test_memory_native_state)
endif()
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_fill_state)
    pineforge_add_unit_test(test_memory_fill_state)
endif()
