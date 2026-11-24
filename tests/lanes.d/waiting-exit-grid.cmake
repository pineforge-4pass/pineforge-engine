if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_waiting_exit_grid)
    pineforge_add_unit_test(test_waiting_exit_grid)
    add_test(NAME test_resting_exit_next_bar
        COMMAND ${Python3_EXECUTABLE} -B -c "import os, sys; os.execv(sys.argv[1], sys.argv[1:])"
            $<TARGET_FILE:test_waiting_exit_grid> resting)
    add_test(NAME test_market_exit_fill_units
        COMMAND ${Python3_EXECUTABLE} -B -c "import os, sys; os.execv(sys.argv[1], sys.argv[1:])"
            $<TARGET_FILE:test_waiting_exit_grid> sizing)
endif()
