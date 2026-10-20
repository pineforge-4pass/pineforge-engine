# CR-150 CX-D-P2: IdPairStore file/arena reference, jail, exhaustion and heap.
if(UNIX)
    list(APPEND TEST_SOURCES test_id_pair_store_arena)
    pineforge_add_unit_test(test_id_pair_store_arena)
endif()
