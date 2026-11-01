# Observe pending-order storage and its retirement directly.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_pending_book)
    pineforge_add_unit_test(test_memory_pending_book)
endif()
