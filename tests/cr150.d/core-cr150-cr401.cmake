# CR-401: both policy variants share test_cr401_static_ta_cache_flag.cpp.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    list(APPEND TEST_SOURCES test_cr401_static_ta_cache_flag)
endif()

# Custom targets/rows require a matching tests/CMakeLists.txt fence under
# check_cr150_cmake_d.py. The proposed fence is in REPORT-cr401-seam.md;
# installing it requires extending this lane's edit scope.
