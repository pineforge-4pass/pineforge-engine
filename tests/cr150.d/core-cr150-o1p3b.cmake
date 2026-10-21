# CR150-CX P3b: only this lane's registrations.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_cr150_p3b_pending_readback)
    pineforge_add_unit_test(test_cr150_p3b_pending_readback)
    list(APPEND TEST_SOURCES test_cr150_p3b_admission_fields)
    pineforge_add_unit_test(test_cr150_p3b_admission_fields)
    list(APPEND TEST_SOURCES test_cr150_p3b_history_view)
    pineforge_add_unit_test(test_cr150_p3b_history_view)
endif()
