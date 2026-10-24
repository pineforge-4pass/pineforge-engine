if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_history_preallocation_a5_100k)
    pineforge_add_unit_test(test_history_preallocation_a5_100k)
    list(APPEND TEST_SOURCES test_history_preallocation_a5_1m)
    pineforge_add_unit_test(test_history_preallocation_a5_1m)
    set_tests_properties(test_history_preallocation_a5_1m PROPERTIES LABELS slow)
    add_test(NAME test_history_frozen2_a5
        COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/a5histdraw_frozen_controls.py
        --kind history --build ${CMAKE_BINARY_DIR} --compiler ${CMAKE_CXX_COMPILER}
        --cmake ${CMAKE_COMMAND} --library $<TARGET_FILE:pineforge>)
    set_tests_properties(test_history_frozen2_a5 PROPERTIES LABELS slow RUN_SERIAL TRUE)
endif()
list(APPEND TEST_SOURCES test_drawing_growth_a5_100k)
pineforge_add_unit_test(test_drawing_growth_a5_100k)
list(APPEND TEST_SOURCES test_drawing_growth_a5_1m)
pineforge_add_unit_test(test_drawing_growth_a5_1m)
set_tests_properties(test_drawing_growth_a5_1m PROPERTIES LABELS slow)
list(APPEND TEST_SOURCES test_drawing_versions_a5)
pineforge_add_unit_test(test_drawing_versions_a5)
add_test(NAME test_drawing_frozen2_a5
    COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/a5histdraw_frozen_controls.py
    --kind drawings --build ${CMAKE_BINARY_DIR} --compiler ${CMAKE_CXX_COMPILER}
    --cmake ${CMAKE_COMMAND} --library $<TARGET_FILE:pineforge>)
set_tests_properties(test_drawing_frozen2_a5 PROPERTIES LABELS slow RUN_SERIAL TRUE)
