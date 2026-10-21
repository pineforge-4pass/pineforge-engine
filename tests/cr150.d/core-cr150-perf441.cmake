# CX-PERF441: canonical hash bytes and fused admission-window retirement.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_admission_fold_perf441)
    pineforge_add_unit_test(test_admission_fold_perf441)
endif()
