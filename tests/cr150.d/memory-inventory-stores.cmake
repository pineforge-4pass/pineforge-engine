# Synthetic inventory checks need no live runner or external fixture.
if(UNIX AND PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_inventory_stores)
    pineforge_add_unit_test(test_memory_inventory_stores)
endif()

list(APPEND TEST_SOURCES test_memory_matcher_scratch)
pineforge_add_unit_test(test_memory_matcher_scratch)
