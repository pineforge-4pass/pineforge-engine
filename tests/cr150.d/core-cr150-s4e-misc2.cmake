# These rows never return a skip sentinel; 77 is the only permitted sentinel.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME test_s4e_misc2_tmpfs COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_run_json_misc2_tmpfs.py)
endif()
add_test(NAME test_s4e_misc2_jailed COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_misc2_test.py -k "jailed_route or unused_strict_default")
add_test(NAME test_s4e_misc2_codes COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_misc2_test.py -k guard)
add_test(NAME test_s4e_misc2_spill_dir COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_misc2_test.py -k explicit_spill_dir)
