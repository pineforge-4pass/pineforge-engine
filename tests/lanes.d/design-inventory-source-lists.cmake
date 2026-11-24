add_test(NAME test_design_inventory_source_lists
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_design_inventory_source_lists.py
            --ctest ${CMAKE_CTEST_COMMAND} --build-dir ${PROJECT_BINARY_DIR})
