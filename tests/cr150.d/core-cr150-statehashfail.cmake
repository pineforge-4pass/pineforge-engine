# CR150-CX: live-store state-hash carried-failure regression.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_state_hash_carried_scratch_failure)
    pineforge_add_unit_test(test_state_hash_carried_scratch_failure)
endif()
