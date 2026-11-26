# The lookup and insertion counters are absent from NDEBUG libraries, so the
# counting row is registered for Debug builds only.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_nested_security_caller)
    pineforge_add_unit_test(test_nested_security_caller)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND TEST_SOURCES test_lower_tf_caller_work)
        pineforge_add_unit_test(test_lower_tf_caller_work)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Count completed scopes in the compiled evaluator (Itanium C++ ABI, D1 destructor).
            target_link_options(test_lower_tf_caller_work PRIVATE
                "-Wl,--wrap=_ZN9pineforge6source24PineRequestDeliveryScopeD1Ev")
            target_compile_definitions(test_lower_tf_caller_work PRIVATE PF_COUNT_DELIVERY_SCOPES)
        endif()
    endif()
endif()
