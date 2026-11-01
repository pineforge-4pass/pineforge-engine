# The global allocator replacement belongs to this executable alone.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_release_orders_closes)
    pineforge_add_unit_test(test_memory_release_orders_closes)
endif()
