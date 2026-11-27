# The delivery-work counters are absent from NDEBUG libraries, so the counting
# row is registered for Debug builds only.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND TEST_SOURCES test_issue_965_delivery_work)
        pineforge_add_unit_test(test_issue_965_delivery_work)
    endif()
endif()
