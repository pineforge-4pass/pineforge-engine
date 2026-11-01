# The warm-up schedule was measured on libstdc++.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_memory_release_adapter_passes)
    pineforge_add_unit_test(test_memory_release_adapter_passes)
endif()
