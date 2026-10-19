
# --- CR-150 core-cr150-l6b2 begin ---
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX)
    add_library(report_sink_equity_strategy MODULE test_report_sink_equity.cpp)
    target_compile_definitions(report_sink_equity_strategy PRIVATE PINEFORGE_L6B2_MODULE=1)
    target_link_libraries(report_sink_equity_strategy PRIVATE pineforge)
    if(APPLE)
        target_link_options(report_sink_equity_strategy PRIVATE
            "-Wl,-force_load,$<TARGET_FILE:pineforge>")
    else()
        target_link_options(report_sink_equity_strategy PRIVATE
            "-Wl,--whole-archive" "$<TARGET_FILE:pineforge>" "-Wl,--no-whole-archive")
    endif()
    target_compile_options(report_sink_equity_strategy PRIVATE -ffp-contract=off -UNDEBUG)
    set_target_properties(report_sink_equity_strategy PROPERTIES
        PREFIX "" SUFFIX ".so" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/report_sink_equity")
    target_link_libraries(test_report_sink_equity PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(test_report_sink_equity PRIVATE
        PINEFORGE_L6B2_MODULE_PATH="$<TARGET_FILE:report_sink_equity_strategy>")
    add_dependencies(test_report_sink_equity report_sink_equity_strategy)
endif()
# --- CR-150 core-cr150-l6b2 end ---
