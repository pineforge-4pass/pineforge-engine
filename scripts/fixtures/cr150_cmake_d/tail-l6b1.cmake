
# --- CR-150 core-cr150-l6b1 begin ---
# Appended after every pre-existing line pin. A real source
# strategy module, with the same whole-archive link as release modules.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    add_library(report_sink_l6b1_strategy MODULE test_report_sink_l6b1.cpp)
    target_compile_definitions(report_sink_l6b1_strategy PRIVATE PINEFORGE_L6B1_MODULE=1)
    target_link_libraries(report_sink_l6b1_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(report_sink_l6b1_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(report_sink_l6b1_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(report_sink_l6b1_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(report_sink_l6b1_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/report_sink_l6b1")
    list(APPEND TEST_SOURCES test_report_sink_l6b1)
    pineforge_add_unit_test(test_report_sink_l6b1)
    target_link_libraries(test_report_sink_l6b1 PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_report_sink_l6b1 PRIVATE
        PINEFORGE_L6B1_MODULE_PATH="$<TARGET_FILE:report_sink_l6b1_strategy>"
        PINEFORGE_L6B1_PYTHON="${Python3_EXECUTABLE}"
        PINEFORGE_L6B1_DOCKER="${PROJECT_SOURCE_DIR}/docker")
    add_dependencies(test_report_sink_l6b1 report_sink_l6b1_strategy)
endif()
# --- CR-150 core-cr150-l6b1 end ---
