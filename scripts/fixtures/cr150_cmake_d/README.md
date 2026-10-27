# Fenced CMake controls

The `lane-*.cmake` and `tail-*.cmake` files copy the real lane inputs supplied
with the core-cr150-cmkd2 brief, based on
`5be8c54e7519a7906f3be7e7a0512e98bdc1cda7`.
Only fence names were normalized: `l6b2`, `l6b3`, `l9a`, and `l17a` become
`core-cr150-l6b2`, `core-cr150-l6b3`, `core-cr150-l9a`, and `core-cr150-l17a`.
L6b1 already uses its full file stem; L19b's unchanged line and comments are
wrapped in `core-cr150-l19b` begin/end markers.
Lane files are copied unchanged.

The supplied `lane-l17a.cmake` is empty and is deliberately not installed:
`tail-l17a.cmake`, including its preceding L6b1 block, is a missing-lane-file
rejection input. L17a must add its own lane file in its next merge round;
the other lanes must rename short fences to their full lane-file stems.
L9a's cross-lane `$<TARGET_FILE:report_sink_l6b1_strategy>` and
`if(TARGET report_sink_l6b1_strategy)` reads remain intact.

The controls construct only a small include-loop tree and source-presence
files; they never configure or build it. The 14 existing lane-property
controls remain, and 41 `FenceControls` methods cover the extension.
Every new refusal control first invokes the checker on a legal fenced write
to `test_owner`. The base checker returns 1 instead of the required 0 for that
write because its last-five-statements rule rejects it. Thus a checker that
rejects every tail cannot satisfy any of the new negative controls.

The table records the static fail-before rationale and expected new result,
not executed proof. No checker, control, preflight, profile, corpus, or docs
run was executed by the edit-only executor. The lab must record the actual
base failures and current passes.

All method names below are under `FenceControls`; the common base failure is
the legal fenced-write assertion described above, except that the positive
cases directly require their own fenced input to succeed.

| Control | Expected current behavior / reason |
| --- | --- |
| `test_fence_before_include_loop_is_refused` | Reject a fence before the loop; the base would accept that malformed placement without the paired positive check. |
| `test_unfenced_statement_after_loop_is_refused` | Reject a target write outside a fence after the loop. |
| `test_begin_without_end_is_refused` | Diagnose the missing end marker. |
| `test_mismatched_fence_names_are_refused` | Require the closing lane name to match the opener. |
| `test_fence_without_lane_file_is_refused` | Reject the real L17a tail for missing `core-cr150-l17a.cmake`, with L6b1's lane file present. |
| `test_foreign_target_writes_are_refused` | Reject every listed target command, multi-target properties, or dependency destination writing `test_other`; quoted/bracketed names keep their ownership and inline bracket comments cannot join destination names. |
| `test_add_compile_options_is_refused` | Reject directory compile options. |
| `test_include_directories_is_refused` | Reject directory include paths. |
| `test_link_libraries_is_refused` | Reject directory link libraries. |
| `test_set_cmake_variable_is_refused` | Reject `set(CMAKE_CXX_FLAGS ...)`. |
| `test_other_directory_and_unknown_commands_are_refused` | Reject directory definitions/link options/link paths/properties, include/file/process/subdirectory commands, and unknown `cmake_language`. |
| `test_duplicate_fence_is_refused` | Allow at most one fence per lane stem. |
| `test_nested_fences_are_refused` | Reject a begin marker inside an active fence. |
| `test_set_property_global_is_refused` | Reject the GLOBAL property scope. |
| `test_set_property_directory_is_refused` | Reject the DIRECTORY property scope. |
| `test_add_test_cannot_claim_another_lanes_test` | Reject re-registering another lane's script test before changing its environment. |
| `test_add_library_cannot_claim_another_lanes_target` | Reject declaring another lane's existing target as a module. |
| `test_foreign_test_properties_are_refused` | Check every test in property commands, including unquoted semicolon lists. |
| `test_fence_cannot_claim_a_target_from_the_main_file` | Reject re-declaration of an existing literal main-file target. |
| `test_short_fence_name_is_not_a_lane_file_stem` | Reject `l6b2` when only `core-cr150-l6b2.cmake` exists. |
| `test_unfenced_statement_between_or_after_fences_is_refused` | Reject writes in gaps and after the final closing marker. |
| `test_variable_and_generator_expression_write_names_are_refused` | Require a literal write destination and a parsed scope; neither expansions nor a hidden list can supply it. |
| `test_adjacent_quoted_write_names_are_refused` | Do not concatenate adjacent quoted destination arguments to conceal a foreign target. |
| `test_bracketed_test_name_keeps_its_creator` | Normalize the ignored opening bracket newline when discovering an existing test's creator. |
| `test_other_target_generator_expressions_are_refused` | Reject external property/linker-file reads, dynamic operators, and nonliteral TARGET_FILE operands. |
| `test_bare_other_lane_target_read_is_refused` | Do not widen the read exception to a bare link dependency. |
| `test_alias_and_imported_targets_are_refused` | Neither an alias nor an imported target establishes lane ownership. |
| `test_dynamic_creation_options_cannot_hide_an_alias` | Reject unresolved constructor options before the first literal source; expanded sources can use `target_sources` on the created target. |
| `test_lane_file_properties_stay_restricted_with_a_fence` | A legal fence does not permit ENVIRONMENT in a lane file. |
| `test_conditional_blocks_must_balance_inside_each_fence` | Reject orphan branches/closers, missing endif, and elseif after else. |
| `test_include_loop_contract_is_preserved` | Still require CONFIGURE_DEPENDS, sorted lanes, the specified foreach list, and the exact include operand. |
| `test_real_l6b1_block_passes` | Accept the actual module, unit-test registration, target settings, and dependency block. |
| `test_real_l6b2_block_passes` | Accept the actual module plus settings of the lane-file-created equity target. |
| `test_real_l6b3_block_passes` | Accept settings of the lane-file-created L6b3 target. |
| `test_real_l9a_block_passes` | Accept the fence-created test and both allowed external-target read forms. |
| `test_real_l19b_block_passes` | Accept the newly fenced Debug definition on the lane-file-created target. |
| `test_real_blocks_pass_together` | Accept all five real fences together, including L9a reading L6b1's module. |
| `test_own_target_and_test_statements_pass` | Accept all supported own-target/test commands, condition branches, and nested CONFIG/TARGET_FILE expressions. |
| `test_ownership_in_other_fence_cannot_be_claimed` | Reject writing another fence's target in either declaration order. |
| `test_fence_markers_in_literal_arguments_are_not_boundaries` | Treat marker and command text inside bracketed or quoted property values as data. |
| `test_fence_markers_in_bracket_comments_are_not_boundaries` | Ignore marker and command text in bracket comments while accepting the real fenced write. |

For lab comparison, save the base checker outside the tree under test and
set `PF_CR150_CMAKE_D_CHECKER` to its absolute path when running
`python3 -B scripts/test_check_cr150_cmake_d.py FenceControls`: all 41 new
methods must fail on that base checker (subtests can report multiple failures).
Unset the override and run the full controls and checker; both must pass,
followed by the brief's preflight, release, debug, and docs verification.

Supervisor commit split: first `scripts/test_check_cr150_cmake_d.py` plus this
fixture directory, then `scripts/check_cr150_cmake_d.py`, then
`tests/cr150.d/README.md`. No new Python script or registration wiring is
needed; `test_cr150_cmake_d_controls` already invokes the extended controls.
