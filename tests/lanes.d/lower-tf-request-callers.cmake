# The lookup and insertion counters are absent from NDEBUG libraries, so the
# counting row is registered for Debug builds only.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_nested_security_caller)
    pineforge_add_unit_test(test_nested_security_caller)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND TEST_SOURCES test_lower_tf_caller_work)
        pineforge_add_unit_test(test_lower_tf_caller_work)
    endif()
endif()
