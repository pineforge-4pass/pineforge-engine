# CR-150 L19D: append after every pre-existing line-pinned registration.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_origin_roster_retirement)
    pineforge_add_unit_test(test_origin_roster_retirement)
endif()
