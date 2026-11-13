add_test(NAME test_corpus_subset_sink_cr150 COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/tests/test_corpus_subset_sink.py)
