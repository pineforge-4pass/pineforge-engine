# The existing source-host fixture accepts real syminfo and auxiliary feeds.
# Forward target references resolve after the existing L6B1 fence.
if(PINEFORGE_BUILD_SOURCE_LAYER AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_library(ordinary_start_recovery_strategy MODULE ${PROJECT_SOURCE_DIR}/tests/ordinary_start_recovery_strategy.cpp)
    target_link_libraries(ordinary_start_recovery_strategy PRIVATE pineforge)
    target_link_options(ordinary_start_recovery_strategy PRIVATE
        "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    target_compile_options(ordinary_start_recovery_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(ordinary_start_recovery_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ordinary_start")
    add_test(NAME test_ordinary_start_witness COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_ordinary_start_witness.py --module $<TARGET_FILE:report_sink_l6b1_strategy> --recovery-module $<TARGET_FILE:ordinary_start_recovery_strategy>)
endif()
