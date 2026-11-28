# Registers tests/report_channel/test_corpus_input_binding.py (the unreadable-input red row and its siblings) so a stage runs it.
add_test(NAME test_report_channel_corpus_input_binding COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider
        ${PROJECT_SOURCE_DIR}/tests/report_channel/test_corpus_input_binding.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
