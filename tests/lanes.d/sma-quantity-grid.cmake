if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_sma_quantity_grid)
    pineforge_add_unit_test(test_sma_quantity_grid)
    list(APPEND TEST_SOURCES test_sma_quantity_tape)
    pineforge_add_unit_test(test_sma_quantity_tape)
    if(DEFINED PINEFORGE_SMA_QUANTITY_CASE)
        add_test(NAME test_sma_quantity_reconstruction_tapes
            COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/sma_quantity_reconstruction.py
                --runner $<TARGET_FILE:test_sma_quantity_grid>
                --case ${PINEFORGE_SMA_QUANTITY_CASE}
                --output ${CMAKE_CURRENT_BINARY_DIR}/sma-quantity-reconstruction)
    endif()
endif()
add_test(NAME test_sma_quantity_intake
    COMMAND ${Python3_EXECUTABLE} -B -m pytest -x -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_sma_quantity_intake.py)
