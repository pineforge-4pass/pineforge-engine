# One-pass report assembly: fused report-sink and bulk bar-loader tests.
# The frozen source fixtures are checked at test-module import.
# Like the corpus proof of the report-sink converters, the opt-in 40-run corpus proof stays outside CI:
# PINEFORGE_ONE_PASS_CORPUS_CASES is supplied only for a lab run.
add_test(NAME test_run_json_fused COMMAND ${Python3_EXECUTABLE} -B -m pytest -q -p no:cacheprovider ${PROJECT_SOURCE_DIR}/docker/run_json_fused_test.py -k "not corpus")
