list(APPEND TEST_SOURCES test_scratch_backing)
pineforge_add_unit_test(test_scratch_backing)
add_test(NAME test_scratch_contract_guard COMMAND ${Python3_EXECUTABLE} -B ${PROJECT_SOURCE_DIR}/tests/test_scratch_contract_guard.py)
# The store fixture links ScratchStore, which the contract headers declare and
# no lane defines yet. Configure with -DPINEFORGE_O1C_STORE_FIXTURE=ON to build
# it and read the link failure (the right red); the lane that defines
# ScratchStore deletes this guard so the fixture runs everywhere.
if(PINEFORGE_O1C_STORE_FIXTURE)
    list(APPEND TEST_SOURCES test_scratch_store)
    pineforge_add_unit_test(test_scratch_store)
endif()
