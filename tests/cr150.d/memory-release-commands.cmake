# Isolated executable: this test replaces every global C++17 new/delete form.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_memory_release_commands)
    pineforge_add_unit_test(test_memory_release_commands)
endif()
