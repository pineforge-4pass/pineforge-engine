# Public fixture: no corpus/submodule or receipt-gated skip path.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_scratch_guard_hl2_01)
    pineforge_add_unit_test(test_scratch_guard_hl2_01)
    set_tests_properties(test_scratch_guard_hl2_01 PROPERTIES LABELS "scratch-io-guard")
endif()
