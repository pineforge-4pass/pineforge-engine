# CR150 A4 family 3a (P26-P31): ordered inventory populations, exact booking
# and the fixed resident bound. P37/P38/P40's admission readback belongs to the
# sibling lane o1-p3b (tests/cr150.d/core-cr150-o1p3b.cmake).
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_a4_ordered_inventory)
    pineforge_add_unit_test(test_a4_ordered_inventory)
endif()
