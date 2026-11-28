# The index lookup counter and reference switch exist only in Debug.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND TEST_SOURCES test_issue_965_ordered_lookup_work)
        pineforge_add_unit_test(test_issue_965_ordered_lookup_work)
    endif()
endif()
