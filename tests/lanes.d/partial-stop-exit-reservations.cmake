if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_test(NAME test_partial_stop_exit_reservations
        COMMAND ${Python3_EXECUTABLE} -B -c "import os, sys; os.execv(sys.argv[1], sys.argv[1:])"
            $<TARGET_FILE:test_waiting_exit_grid> partial)
endif()
