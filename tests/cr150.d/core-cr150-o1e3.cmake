# CR-150 core-cr150-o1e3: the consumer inventory family (E06-E09, E11, E12).
# The paged scratch is a POSIX store (mkstemp/pread/pwrite, like the native
# successor-roots index), so the row is registered where that storage exists.
if(UNIX)
    list(APPEND TEST_SOURCES test_o1e3_consumer_pages)
    pineforge_add_unit_test(test_o1e3_consumer_pages)
endif()
