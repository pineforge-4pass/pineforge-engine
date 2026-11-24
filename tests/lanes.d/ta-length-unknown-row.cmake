add_test(NAME test_ta_length_unknown_row
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ta_length_unknown_row.py $<TARGET_FILE:test_ta_length_int_limit>)
