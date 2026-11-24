# A later partial percentage exit placed while flat behind a waiting partial
# percentage definition (a follow-up row of issue #810 Case A).
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_issue810_later_partial_clip)
    pineforge_add_unit_test(test_issue810_later_partial_clip)
endif()
