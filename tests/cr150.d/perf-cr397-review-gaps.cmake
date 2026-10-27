# Included by the existing sorted cr150.d glob. TARGET_FILE resolves after
# perf-cr397-c2's later fence declares the real whole-archive strategy module.
# Separate CTest rows keep each population/corner/priority pair observable.
if(PINEFORGE_BUILD_SOURCE_LAYER AND UNIX AND NOT PINEFORGE_ENABLE_SANITIZERS AND NOT PINEFORGE_ENABLE_COVERAGE)
    add_test(NAME test_run_json_native_sink_review_f64_population
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_fragment_population")
    add_test(NAME test_run_json_native_sink_review_f64_digit
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_wire_perturbations[digit]")
    add_test(NAME test_run_json_native_sink_review_f64_trailing_zero
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_wire_perturbations[trailing-zero]")
    add_test(NAME test_run_json_native_sink_review_f64_uppercase_e
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_wire_perturbations[uppercase-E]")
    add_test(NAME test_run_json_native_sink_review_f64_plus
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_wire_perturbations[plus]")
    add_test(NAME test_run_json_native_sink_review_f64_spelling_controls
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_f64_review.py::test_f64_spelling_controls")
    add_test(NAME test_run_json_native_sink_review_guard_window_65535
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_escaped_tag_window[65535]")
    add_test(NAME test_run_json_native_sink_review_guard_window_65536
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_escaped_tag_window[65536]")
    add_test(NAME test_run_json_native_sink_review_guard_window_65537
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_escaped_tag_window[65537]")
    add_test(NAME test_run_json_native_sink_review_guard_skipped_over_1mib
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_skipped_line_over_one_mib")
    add_test(NAME test_run_json_native_sink_review_guard_escaped_cap_lf
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_escaped_tag_cap_plus_one[lf]")
    add_test(NAME test_run_json_native_sink_review_guard_escaped_cap_quote
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_escaped_tag_cap_plus_one[quote]")
    add_test(NAME test_run_json_native_sink_review_last_record
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_last_record_only_defect")
    add_test(NAME test_run_json_native_sink_review_empty_zero
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_empty_sink_zero_counts")
    add_test(NAME test_run_json_native_sink_review_empty_wrong_count
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_empty_sink_wrong_count")
    add_test(NAME test_run_json_native_sink_review_pair_2
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_two_defect_source_priority[2]")
    add_test(NAME test_run_json_native_sink_review_pair_5
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_two_defect_source_priority[5]")
    add_test(NAME test_run_json_native_sink_review_pair_11
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_two_defect_source_priority[11]")
    add_test(NAME test_run_json_native_sink_review_pair_13
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_two_defect_source_priority[13]")
    add_test(NAME test_run_json_native_sink_review_pair_g2
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_two_defect_guard_g2")
    add_test(NAME test_run_json_native_sink_review_guard_identity_framing
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_guard_identity_before_header_framing")
    add_test(NAME test_run_json_native_sink_review_i53_control
        COMMAND ${CMAKE_COMMAND} -E env "PINEFORGE_CR397_GENERATED_SO=$<TARGET_FILE:cr397_generated_strategy>"
        ${Python3_EXECUTABLE} -B -m pytest -q -s -x -p no:cacheprovider
        "${PROJECT_SOURCE_DIR}/tests/test_run_json_native_sink_guard_review.py::test_i53_token_control")
endif()
