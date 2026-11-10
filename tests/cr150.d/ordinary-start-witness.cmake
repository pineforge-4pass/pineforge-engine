# The existing source-host fixture accepts real syminfo and auxiliary feeds.
# Forward target references resolve after the existing L6B1 fence.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_ordinary_start_witness COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_ordinary_start_witness.py --module $<TARGET_FILE:report_sink_l6b1_strategy>)
endif()
