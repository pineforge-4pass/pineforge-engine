
# --- lane report-sink-record-kinds begin ---
if(UNIX)
    target_compile_definitions(test_report_sink_l6b3 PRIVATE
        PINEFORGE_L6B3_PYTHON="${Python3_EXECUTABLE}"
        PINEFORGE_L6B3_DOCKER="${PROJECT_SOURCE_DIR}/docker")
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        target_compile_definitions(test_report_sink_l6b3 PRIVATE PINEFORGE_L6B3_SOURCE=1)
    endif()
endif()
# --- lane report-sink-record-kinds end ---
