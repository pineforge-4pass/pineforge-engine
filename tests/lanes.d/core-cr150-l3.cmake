# CR-150 L3: append after every pre-existing line-pinned registration.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_chart_store_once)
    pineforge_add_unit_test(test_chart_store_once)
endif()
