add_test(NAME test_abi_receipt_commands
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_abi_receipt_commands.py
            --ctest ${CMAKE_CTEST_COMMAND} --build-dir ${PROJECT_BINARY_DIR})
