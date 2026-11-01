# Check spilled-book reads, ring overwrite and input-sized payloads.
list(APPEND TEST_SOURCES test_memory_book_reader_ring)
pineforge_add_unit_test(test_memory_book_reader_ring)
