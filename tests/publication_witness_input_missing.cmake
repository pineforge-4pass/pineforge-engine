# The row test_publication_witness becomes when the private fixtures are in the
# tree but a file the witness is built from is not: its translation unit, or the
# pinned table that unit includes. tests/CMakeLists.txt registers this driver in
# that one case and passes the missing file as PF_MISSING_INPUT.
#
# It always fails, by exit status, and its first line carries the code and the
# missing file. The row is then never a skip and never a pass.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PF_MISSING_INPUT OR PF_MISSING_INPUT STREQUAL "")
    set(PF_MISSING_INPUT "the registration named no file")
endif()

message("PF_PUBLICATION_WITNESS_INPUT_MISSING: ${PF_MISSING_INPUT}")
message(FATAL_ERROR
    "test_publication_witness cannot run: the private fixtures are in this "
    "tree, but the witness's own input named above is not. Restore it and "
    "configure again.")
