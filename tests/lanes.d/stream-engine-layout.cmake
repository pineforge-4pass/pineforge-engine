# The engine layout the live runner shares with a stamped strategy module, against its measured pins. The pins are measured on Linux x86_64: no other machine registers the row.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    list(APPEND TEST_SOURCES test_stream_engine_layout)
    pineforge_add_unit_test(test_stream_engine_layout)
endif()
