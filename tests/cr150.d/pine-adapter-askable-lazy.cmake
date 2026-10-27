# Retirement origin construction and membership are deferred until needed.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    list(APPEND TEST_SOURCES test_pine_adapter_askable_lazy)
    pineforge_add_unit_test(test_pine_adapter_askable_lazy)
endif()
