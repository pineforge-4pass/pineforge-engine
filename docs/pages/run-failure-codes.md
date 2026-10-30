# Run-failure codes {#run_failure_codes}

@tableofcontents

A run that fails leaves two things on its handle: the English text
#strategy_get_last_error has always returned, unchanged byte for byte, and a
**stable code** from a closed vocabulary with its **typed arguments**. The
text is for a person. The code is for a program: a harness that retries,
bills or refunds by the class of a failure, or a localizing consumer that
shows it in the reader's language. And a script cannot forge it: whatever a
script prints with `runtime.error`, its code reads `strategy_runtime_error`,
so copying another failure's English never claims that failure's class.

The vocabulary is `docker/run_failure_codes.json` (schema
`pineforge-run-failure-catalog/v1`). Every release attaches it, and its
changes since the release its notes start at (for a final release, the
previous final release), as release assets
([below](#run_failure_codes_release)).

## Reading a failure {#run_failure_codes_getters}

```c
#include <pineforge/pineforge.h>

const char* text = strategy_get_last_error(s);       /* "" when no failure    */
const char* code = strategy_get_last_error_code(s);  /* "" when no failure    */
const char* args = strategy_get_last_error_args(s);  /* "" when no failure,
                                                        "{}" with no arguments */
if (code != NULL && code[0] != '\0') {
    /* the most recent run (or setter) on s failed: dispatch on code, read args */
}
```

- #strategy_get_last_error_code answers `""` exactly when the most recent run
  or setter on the handle recorded no failure, and `NULL` only for a `NULL`
  handle. A code is lower snake_case ASCII matching `^[a-z][a-z0-9_]{2,47}$`.
  It is not keyed on the text: a script stopped by `runtime.error()` with an
  empty message fails with the text `""` and the code `strategy_runtime_error`,
  and a non-empty text that no coded site wrote reads
  `engine_unclassified_error`, never `""`.
- #strategy_get_last_error_args answers the arguments as one canonical JSON
  object: UTF-8, keys sorted, no whitespace; an `integer` argument is a JSON
  integer, a `number` the shortest round-trip double, a `boolean` a JSON
  `true` or `false`, a `nullable_string` a
  JSON string or `null`, a `scalar` a JSON string, integer, finite number,
  `true`, `false` or `null` (a boolean or a null is never written as a
  string), every other kind a JSON string. It is `"{}"` for a code without
  arguments and `""` when there is no failure.
- Both pointers live as long as #strategy_get_last_error's: until the next run
  or setter on the handle.
- Run each request on a fresh handle. A handle whose run failed, a script
  stop included and an aborted run excepted, refuses its next run with `Pine
  native adapter failed to configure projected run spec` (`engine_invariant`),
  as it did before the codes.
- `PINEFORGE_HAS_RUN_FAILURE_CODES_V1` (defined by `<pineforge/pineforge.h>`
  and `<pineforge/run_failure.hpp>`) is the compile-time probe. At run time a
  harness looks the two symbols up (`dlsym`, Python's `hasattr`): a strategy
  library built against an earlier runtime lacks them. The getters are
  additive exports and move no layout; the current `PF_ABI_VERSION` is 5.

A failed run is read from the code, never from the text alone.
`docker/run_json.py` fails a run when the code is non-empty, the text is
non-empty or #strategy_last_run_status is `1`, and prints the code and the
arguments on its error line (`docker/README.md` gives the line). Before the
codes it tested the text alone, so a script stopped by an empty
`runtime.error()` printed a success report of the bars it had reached.

## The rules every code keeps {#run_failure_codes_rules}

- **A code is chosen where the failure is raised, never read from data.** The
  engine, the Pine library and the transpiler's generated stops each name
  their code at the throw or the refusal. `runtime.error` is
  `strategy_runtime_error` whatever its message.
- **An argument never carries a value the script computed while it ran.** An
  argument, apart from the trusted resource measurements below, comes only
  from a literal the transpiler emitted (a call's spelling, its line, a
  literal symbol, an input title), from a closed engine vocabulary (a field,
  a reason, a limit), or from a value the run request supplied (a row of its
  OHLCV, its timeframes). Script-computed indexes, sizes, symbols or
  timeframes, the bar a failure happened on and any text a script built are
  not arguments, so a failing run cannot carry results out through them.
- **Resource measurements are an explicit allowance.** Like the existing
  sink-exhaustion arguments, admitted caps, trusted planned representation
  sizes, accepted byte counts and overflow probes are measured boundary
  facts. The supervisor or finalizer produces these arguments; they are
  never parsed from a strategy's message or accepted as arbitrary
  script-computed values.
- **Cancellation progress is engine-owned.** Q7's `run_aborted` witnesses
  are the count of fully completed input bars and a boolean start witness.
  They are produced by the run boundary, never read from script output.
- **Validation is central.** An undeclared argument, a wrong kind, a value
  outside an argument's closed list or a missing required argument turns the
  failure into `engine_invariant`, its English kept: a wrong call cannot mint
  a code or an open string.
- **A failure without a code is classified by its type, as a safety net
  only.** `std::bad_alloc` is `out_of_memory`, the `std::logic_error` family
  is `engine_invariant`, and any other exception is
  `engine_unclassified_error`.
- **The English is unchanged.** Every existing text stays byte for byte; a
  failure that had no text gets one only where it had none.

## Classes, codes and argument kinds {#run_failure_codes_list}

The tables below list the catalog at this tree.

The catalog contains 79 named failure codes: 38 first released in 1.4.0,
26 first released in 1.5.0, 5 added in 1.7.0, and 10 added in 1.9.0
(3 run-quota codes and 7 report-channel codes). One unnamed `none` sentinel
gives 80 registry entries; existing numbers stay fixed.

The run-quota additions are `run_descriptor_limit_exceeded` (74),
`run_quota_configuration_invalid` (78), and `run_resource_quota_exceeded`
(79). Their finite contract is described [below](#run_failure_codes_quota).

The seven new codes describe the planned supervised report channel. The
harness does not emit them yet, and their registration declares no
report-channel capability. Their closed vocabularies and measurement rules
are described [after the tables](#run_failure_codes_report_channel).
The registrations are `diagnostic_budget_exceeded` (70),
`report_budget_exceeded` (71), `report_channel_failed` (72),
`run_deadline_exceeded` (73), `run_isolation_unavailable` (75),
`run_process_refused` (76), and `run_producer_failed` (77).

The 1.7.0 additions are `report_mode_unsupported`, `report_sink_failed`,
`scratch_capacity_exceeded`, `scratch_configuration_invalid`, and
`scratch_io_failed`. Their classes, retryability and typed arguments are
listed below.

`report_mode_unsupported` (code 65) also covers a native begin when
`SUMMARY_ONLY` (the v1 mode or the pending legacy equity mode) is combined with
an execution observer or a selected window and no report sink is enabled:
that capture cannot be exact in this mode. Begin refuses before callbacks or
bars. Drop the window or the observer, or select `FULL`; bounded capture uses
`STREAMED` with a sink.

The 26 codes first released in 1.5.0 comprise `outputs_limit`,
`outputs_rejected`, and the 24 selected-window and report-contract rows
described [after the tables](#run_failure_codes_selected_window).
The `strategy_library_incompatible` row also includes the two released
1.5.0 outputs reasons, `outputs_api_mismatch` and `outputs_manifest_invalid`,
in addition to its six released 1.4.0 reasons.

<!-- BEGIN generated from docker/run_failure_codes.json: python3 scripts/test_gen_run_failure_catalog_diff.py --write-docs-tables -->

### Classes

| Class | What failed |
| --- | --- |
| `engine_fault` | A PineForge bug; terminal. |
| `input` | The run request's own data or options; terminal. |
| `no_data` | PineForge lacks data the script reads; terminal. |
| `resource` | Machine memory, scratch storage, host process or transport availability and budgets, including an unavailable producer; retryability is specified per code. |
| `strategy` | The script or its own declaration; terminal. |
| `strategy_limit` | A TradingView or PineForge resource cap the script reached; terminal. |
| `symbol_feeds` | Other symbols' bars installed through the symbol-feed setters; terminal. |
| `symbol_metadata` | The symbol's metadata the caller supplied; retryable once the caller refreshes it. |
| `unsupported` | A construct or option PineForge does not support for this run; terminal. |

### Codes

| Code | Class | Retryable | Arguments | What it means |
| --- | --- | --- | --- | --- |
| `chart_bars_rejected` | `input` | no | `field` (vocab, 6 values, optional), `index` (integer, optional), `reason` (vocab, 12 values) | The run's chart bars (the request's OHLCV) were refused. |
| `chart_bars_unreadable` | `input` | no | `reason` (vocab, 9 values) | The harness could not read the run's OHLCV file. |
| `diagnostic_budget_exceeded` | `resource` | no | `cap_bytes` (integer), `requested_bytes` (integer), `used_bytes` (integer) | The diagnostics exceed their admitted byte budget. |
| `engine_command_value_invalid` | `engine_fault` | no | `field` (string), `reason` (vocab, 3 values), `stage` (vocab, 7 values) | The engine derived an invalid command value (non-finite, invalid text or an overflowing finite value) from its own book, equity, margin, sizing, fee, slippage or settlement state: an engine fault, never a strategy failure. stage names where it was derived. |
| `engine_invariant` | `engine_fault` | no | `reason` (vocab, 1 values, optional) | An engine invariant failed: a PineForge bug. |
| `engine_unclassified_error` | `engine_fault` | no | none | An error no coded site reported: a PineForge bug. |
| `harness_internal_error` | `engine_fault` | no | none | The run harness failed: a PineForge bug. |
| `lot_grid_rejected` | `symbol_metadata` | yes | none | The symbol's lot size (syminfo.mincontract, the quantity grid) is not a value the engine accepts. |
| `no_data_request` | `no_data` | no | `call` (pine_source), `function` (vocab, 9 values), `line` (integer) | The script read a request whose data PineForge does not pin for this run. |
| `other_symbol_feed_missing` | `no_data` | no | `timeframe` (timeframe, optional) | No feed is installed for another symbol's request at its timeframe. |
| `other_symbol_request` | `no_data` | no | `call` (pine_source), `function` (vocab, 2 values), `line` (integer), `symbol` (symbol, optional) | The script read another symbol's request and no bars of that symbol were installed. |
| `out_of_memory` | `resource` | yes | none | A C++ allocation failed, including scratch caches, arena metadata, store workspace, owning journal decode or diagnostic construction. A real syscall ENOMEM retains its I/O code. |
| `outputs_limit` | `strategy_limit` | no | `max` (integer), `reason` (vocab, 2 values) | A recorded-outputs capacity limit was reached during the run. |
| `outputs_rejected` | `input` | no | `reason` (vocab, 3 values) | Recorded outputs were asked of a strategy library that records none, or the recording cannot be honoured as asked. |
| `pine_array_error` | `strategy` | no | `collection` (vocab, 2 values, optional), `method` (vocab, 12 values, optional), `reason` (vocab, 5 values) | An array call failed: an index out of bounds, an inverted slice, an empty array, an invalid size, or a modified historical collection. |
| `pine_invalid_argument` | `strategy` | no | `argument` (identifier, optional), `function` (identifier, optional), `rule` (vocab, 6 values) | A built-in refused the value of one of its arguments. |
| `pine_matrix_error` | `strategy` | no | `function` (vocab, 15 values), `reason` (vocab, 10 values) | A matrix built-in refused its arguments (an index, a dimension, a size or an empty matrix). |
| `pine_na_reference` | `strategy` | no | `object` (vocab, 10 values) | The script called a method on an na reference (an array, matrix, map, drawing or object id). |
| `pine_runtime_limit` | `strategy_limit` | no | `limit` (vocab, 4 values), `max` (integer, optional) | The script reached a TradingView or PineForge resource limit. |
| `pine_string_error` | `strategy` | no | `reason` (vocab, 2 values) | A string built-in refused its arguments. |
| `recalc_cap` | `strategy_limit` | no | none | The script's calc_on_order_fills recalculation loop reached its cap. |
| `recorded_request_missing` | `no_data` | no | none | No recorded series is installed for a request the script reads. |
| `recorded_request_refused` | `input` | no | `reason` (vocab, 5 values) | A recorded request series was refused. |
| `report_budget_exceeded` | `resource` | no | `cap_bytes` (integer), `channel` (vocab, 2 values), `requested_bytes` (integer), `used_bytes` (integer) | The report exceeds its admitted byte budget. |
| `report_channel_failed` | `resource` | no | `channel` (vocab, 5 values), `reason` (vocab, 5 values) | A report transport could not complete. |
| `report_mode_unsupported` | `unsupported` | no | `contract` (string), `mode` (string) | The requested report mode cannot be provided for this run. Two causes: the host has no provider bridge for the mode and contract (run_json), or the native kernel refuses a SUMMARY_ONLY run that configures a selected window or an execution observer without a report sink at begin, because that capture has no exact trade rows. Remedy for the second cause: drop the window or the observer, or select FULL; bounded capture uses STREAMED with a sink. |
| `report_platform_unavailable` | `engine_fault` | no | `field` (vocab, 6 values) | The runner cannot establish an execution-platform fact its report contract requires. A pre-load backstop for an image that claimed results-digest support, raised before any strategy code loads. |
| `report_post_execution_failed` | `engine_fault` | no | `phase` (vocab, 3 values), `reason` (vocab, 4 values) | The engine could not finish producing the result after strategy execution completed: a deadline or an otherwise unclassified failure in result assembly, the results digest or serialization. An engine fault, never a strategy failure; a specific typed engine fault keeps its own code. |
| `report_result_invalid` | `engine_fault` | no | `field` (string), `reason` (vocab, 4 values) | The report producer generated an invalid canonical result: an engine or harness fault, never a strategy failure. |
| `report_shape_request_invalid` | `input` | no | `option` (vocab, 2 values), `value` (scalar) | The report-shape request is invalid: the report shape or the curve point budget carries a value the runner refuses. |
| `report_shape_unsupported` | `unsupported` | no | `shape` (string) | The runner does not support the requested report shape. The first selected-window build supports the full shape only. |
| `report_sink_failed` | `engine_fault` | no | `arena_bytes` (integer, optional), `cap_bytes` (integer, optional), `claimed_bytes` (integer, optional), `high_water_bytes` (integer, optional), `needed_bytes_lower_bound` (integer, optional), `reason` (vocab, 7 values, optional), `sink_bytes` (integer, optional), `store` (vocab, 13 values, optional) | The engine could not complete the report sink, scratch storage or spill arena. Every reason, including exhaustion of the engine-certified arena bound or planned sink cap and a closed sink peer during finalization, is an engine fault. reason distinguishes arena exhaustion (arena_exhausted), arena I/O (arena_io), invalid arena backing (arena_backing), file-mode scratch I/O (scratch_io), sink open/write I/O (sink_io), a closed sink peer (sink_peer_closed) and a finalizer stream exceeding its planned cap (sink_exhausted). With arena_exhausted, high_water_bytes and arena_bytes are byte counts, claimed_bytes counts chunk bytes claimed when the failing claim was attempted, and store identifies the requesting scratch user. needed_bytes_lower_bound is the requesting report region's additional whole-chunk demand. For store command it is the arena size needed for the required store extent rounded up to chunks, divided by kCommandScratchArenaShare and rounded up to bytes (saturated at INT64_MAX). Crossing a command store's share is arena_exhausted and the hosting layer re-places the run. With sink_exhausted, the finalizer exits 8; sink_bytes is the bytes read (cap_bytes + 1) and cap_bytes is the planned cap U. APP maps that receipt to this failure and refunds the run; COMPUTE re-places it with 2x U. Other reasons carry no sizes or store identity. All arguments are optional for existing raisers; the run has no completed sink or valid trailer. |
| `request_symbol_invalid` | `strategy` | no | none | A request.security symbol is not a valid symbol. |
| `request_timeframe_invalid` | `strategy` | no | `api` (vocab, 2 values) | A request timeframe literal does not parse. |
| `request_timeframe_unsupported` | `unsupported` | no | `input_tf` (timeframe, optional), `reason` (vocab, 7 values), `script_tf` (timeframe, optional) | A request timeframe cannot be served for this run's chart timeframe. |
| `request_unsupported` | `unsupported` | no | `line` (integer, optional), `reason` (vocab, 2 values) | A request shape PineForge does not support. |
| `results_digest_unsupported` | `unsupported` | no | `requested` (string) | The runner does not support the requested results digest. The first selected-window build declares no digest. |
| `run_aborted` | `engine_fault` | yes | `bars_count` (integer, optional), `strategy_started` (boolean, optional) | The run was aborted on request. Cooperative SIGTERM stops after the in-flight input bar completes, leaves no sink trailer, and exits 1. bars_count is the nonnegative number of fully completed input bars, mirrored as top-level bars.count, matching the runner's bars_processed population; strategy_started is a boolean mirrored at the top level. Both are optional for legacy abort raisers. There is no cancelled class: the abort path has exactly two internal callers, the orchestrator's hand-off at the service limit (the run is restarted with the same run id) and an operator stop. The abort line itself carries no retry semantics: it has no retryable field, and a retry is APP's decision from which caller stopped the run. On the abort path bars_count is diagnostic only and is never billed. No live-counter descriptor is used (fd 6 is not used). |
| `run_deadline_exceeded` | `resource` | no | `stage` (vocab, 5 values) | The admitted channel lifetime expired. |
| `run_descriptor_limit_exceeded` | `resource` | no | `limit` (integer), `peak` (integer), `process_role` (vocab, 6 values), `requested` (integer), `soft_nofile` (integer), `stage` (vocab, 8 values), `used` (integer) | Descriptor admission exceeded the finite run allowance or the process soft NOFILE limit. used includes committed and pending descriptors before the denied additional requested count; peak is the preceding high water. Counters are host-owned nonnegative integers at most 2^53-1. |
| `run_isolation_unavailable` | `unsupported` | no | `reason` (vocab, 3 values) | The requested report isolation is unavailable. |
| `run_mode_unsupported` | `unsupported` | no | `feature` (vocab, 11 values) | A feature the requested run mode does not support. |
| `run_options_rejected` | `input` | no | `input_tf` (timeframe, optional), `option` (vocab, 11 values), `script_tf` (timeframe, optional) | The run's own options (timeframes, intrabar path, magnifier, path order) were refused. |
| `run_process_refused` | `resource` | no | `operation` (vocab, 4 values) | The native process attempted forbidden process creation. |
| `run_producer_failed` | `resource` | no | `exit_status` (integer, optional), `reason` (vocab, 3 values), `signal` (integer, optional) | The native report producer did not complete successfully. |
| `run_quota_configuration_invalid` | `input` | no | `reason` (vocab, 8 values), `setting` (vocab, 10 values), `stage` (vocab, 8 values) | The finite host run contract is missing, malformed, overflowing, unauthenticated or unsupported. An unset limit refuses with missing_limit; explicit decimal zero is literal zero capacity. There is no implicit unlimited mode. Limits, extents, deltas and reportable counters must fit 2^53-1. |
| `run_request_invalid` | `input` | no | `option` (vocab, 25 values) | The harness command line or one of its JSON options is invalid. |
| `run_resource_quota_exceeded` | `resource` | no | `artifact` (vocab, 14 values), `limit` (integer), `peak` (integer), `requested` (integer), `resource` (vocab, 3 values), `stage` (vocab, 8 values), `used` (integer) | An atomic reservation exceeded a finite run resource allowance before growth. used is committed plus pending before the denied delta; requested is additional capacity, not the desired total; peak is the preceding high water. Produced output is never refunded by deleting an artifact. Counters are host-owned nonnegative integers at most 2^53-1. |
| `scratch_capacity_exceeded` | `resource` | no | `resource` (vocab, 10 values) | The admitted scratch-table layout or a fixed region's capacity was exceeded. The run must be admitted with sufficient finite maxima before retrying. |
| `scratch_configuration_invalid` | `input` | no | `reason` (vocab, 3 values), `stage` (vocab, 1 values) | The command-scratch byte cap from the environment or the caller is malformed: not a positive decimal, out of uint64_t range, or zero. |
| `scratch_io_failed` | `resource` | no | none | The engine could not create, resize, read or write command scratch or the storage of one of its auxiliary tables (the disk id table, the id-pair store or the placement table). Command scratch also raises this code when the default-directory policy fails a directory query or refuses memory-backed default candidates, or when an explicit memory-backed command directory is refused. Native runs create command scratch at begin, so these failures are resource failures before any strategy callback. The run has no successful completion. Report-sink scratch files, the curve writer and the position-tail facade do not raise this code: they raise report_sink_failed with reason scratch_io. |
| `security_feed_refused` | `input` | no | `reason` (vocab, 11 values) | An auxiliary or native request.security feed was refused. |
| `setting_rejected` | `input` | no | `entrypoint` (vocab, 4 values, optional), `input` (pine_source, optional), `reason` (vocab, 21 values, optional) | A run setting (an input or a strategy() override) was refused. |
| `setting_unsupported` | `unsupported` | no | none | A setting this compiled strategy cannot honour. |
| `strategy_create_failed` | `engine_fault` | no | none | The compiled strategy could not be created. |
| `strategy_library_incompatible` | `engine_fault` | no | `abi` (integer, optional), `missing` (vocab, 13 values, optional), `reason` (vocab, 8 values) | The compiled strategy library does not match the harness. |
| `strategy_runtime_error` | `strategy` | no | none | The script stopped itself with runtime.error(); the message, possibly empty, is the text. |
| `strategy_settings_rejected` | `strategy` | no | `field` (vocab, 30 values) | A strategy() declaration setting (or its override) is not a value the engine accepts. |
| `strategy_value_invalid` | `strategy` | no | `field` (string), `reason` (vocab, 3 values) | The strategy supplied an invalid actionable value (a non-finite number, invalid text or an overflowing finite value) to a declaration, setting or command that needs a valid one. |
| `stream_input_rejected` | `input` | no | none | A streaming input (bar, tick or auxiliary bar) was refused. |
| `symbol_feeds_refused` | `symbol_feeds` | no | `field` (vocab, 6 values, optional), `input_tf` (timeframe, optional), `reason` (vocab, 47 values), `script_tf` (timeframe, optional) | Other symbols' bars installed for request.security were refused. |
| `symbol_metadata_rejected` | `symbol_metadata` | yes | `field` (vocab, 14 values) | The symbol's catalog metadata (syminfo) is not a value the engine accepts. |
| `syminfo_unreadable` | `symbol_metadata` | yes | `reason` (vocab, 4 values) | The harness could not read the run's syminfo file. |
| `window_boundary_unaligned` | `input` | no | `bound` (vocab, 3 values), `next_boundary_ms` (integer), `previous_boundary_ms` (integer), `value_ms` (integer) | A selected-window boundary (start, end or fed start) is not aligned to the script calendar; the adjacent boundaries are carried. |
| `window_calendar_unsupported` | `unsupported` | no | `session` (nullable_string), `timezone` (string) | The timezone and session the selected window needs make a calendar the engine does not support; session may be null. |
| `window_feed_identity_changed` | `input` | no | `actual_sha256` (string), `expected_sha256` (string), `feed_id` (string) | The frozen primary feed's bytes differ from the SHA-256 the study recorded. The two digests are 64-character hexadecimal strings supplied by the trusted wrapper; the registry checks only that they are text. |
| `window_feed_range_invalid` | `input` | no | `end_ms` (integer), `fed_start_ms` (integer), `time_ms` (integer) | A primary-feed bar lies outside the frozen selected-window range. |
| `window_input_cap_exceeded` | `input` | no | `actual` (integer), `cap_name` (vocab, 8 values), `limit` (integer) | The supplied history exceeds a configured input cap; cap_name says which. |
| `window_legacy_flag_conflict` | `input` | no | `option` (string) | The legacy trade-start option was combined with selected-window mode. |
| `window_lineage_mismatch` | `input` | no | `actual` (scalar), `expected` (scalar), `field` (string) | A requested window field differs from the study's lineage. |
| `window_mode_unsupported` | `unsupported` | no | `capability` (string), `requested` (integer, optional), `timeframe` (string, optional) | The runner does not support the requested selected-window mode or a limit its build declares on it. capability names what is unsupported; timeframe carries the offending canonical timeframe and requested the offending pre-roll length where the refusal names one. Which optional argument goes with which capability is the trusted raisers' duty, not the registry's. |
| `window_no_observations` | `input` | no | `end_ms` (integer), `start_ms` (integer) | The selected window holds no observations to score. |
| `window_preroll_out_of_range` | `input` | no | `requested` (integer) | The requested pre-roll is outside the range of zero to 5000 script bars. |
| `window_prestart_fill` | `engine_fault` | no | `fill_ms` (integer), `start_ms` (integer) | A fill was dated before the selected window: an engine invariant failure. |
| `window_prestart_state_violation` | `engine_fault` | no | `position_size` (number), `resting_orders` (integer), `time_ms` (integer) | A position or a resting order existed before the selected window started: an engine invariant failure. |
| `window_pruner_unsupported` | `unsupported` | no | `pruner` (string) | A study that requests a selected window together with a trial pruner other than none is refused before any trial is allocated; pruner is the requested pruner's name as the caller sent it. Optimizer-only: run_json never raises it, because a backtest has no pruner. |
| `window_range_invalid` | `input` | no | `end_ms` (integer), `fed_start_ms` (integer), `start_ms` (integer) | The selected-window range (start, end, fed start) is invalid. |
| `window_report_mismatch` | `engine_fault` | no | `actual` (integer), `expected` (integer), `field` (vocab, 3 values) | The report disagrees with the trusted window counts. Raised by the caller that compares them and never emitted by run_json. |
| `window_request_invalid` | `input` | no | `option` (vocab, 13 values) | The selected-window request is invalid: option names the missing, malformed or conflicting field. |

### Argument kinds

| Kind | Its value |
| --- | --- |
| `boolean` | A JSON boolean. |
| `identifier` | A name the compiled strategy spells as a literal (a built-in, an argument). |
| `integer` | A JSON integer. |
| `keyword` | A fixed keyword. |
| `nullable_string` | UTF-8 text, or null when the request value is absent. |
| `number` | A JSON number (shortest round-trip double). |
| `pine_source` | Source text the transpiler emitted as a literal (a call's spelling, an input title). |
| `scalar` | A request value: UTF-8 text, an integer, a finite number, true, false or null. |
| `string` | UTF-8 text the trusted raiser supplies (a request value or a fixed name). |
| `symbol` | A symbol the script wrote as a literal. |
| `timeframe` | A timeframe the run request supplied. |
| `vocab` | One value of the arg's closed `values` list. |

<!-- END generated tables -->

A code's closed value lists and its English templates are the catalog's own
`values` and `english` arrays. A `vocab` argument takes exactly one of its
values. `retryable` says whether the same request can succeed when it is run
again: after memory frees up, once the caller refreshes the symbol's
metadata, or after an aborted run.

`report_sink_failed.store` also includes `command` for an attached arena's
command/exit scratch region. Its `arena_exhausted` exception keeps the original
allocator counters when a command-store owner rethrows it. Crossing the
per-store arena share carries this same typed verdict, with needed bytes
computed as the arena size satisfying that store's required extent. A separate
command byte-budget refusal retains its status-only I/O behavior.

File-backed scratch must have disk backing. With no scratch directory configured, the engine checks filesystem type with `statfs`,
using nonempty `TMPDIR` if disk-backed, or the legacy `/tmp` candidate when `TMPDIR` is unset or empty. Only a successful query proving
that first candidate is tmpfs/ramfs permits trying `/var/tmp`, also checked by filesystem type. A failed query stops resolution with its
syscall errno and names the failed candidate, except as described below. Directory names are not a medium test; tmpfs/ramfs is never silently chosen.
If both default candidates are proven tmpfs/ramfs, the engine retains `ENODEV` / `scratch requires disk backing`, names every candidate tried and its rejection reason in the failure detail, and adds `set TMPDIR to a disk-backed directory`.

An ENOTSUP probe gives no filesystem evidence: keep the candidate, initially the legacy TMPDIR-or-/tmp choice (error 0, no tmpfs claim, no refusal note).
This covers hosts with no probe (neither Linux nor macOS) and real ENOTSUP from stat/faccessat/statfs; the test is by errno value,
so on Linux ENOTSUP and EOPNOTSUPP are both 95. A FUSE/9p/network mount answering 95 keeps its candidate; a later create failure
keeps the plain create text without a note. If the first candidate is proven tmpfs/ramfs and /var/tmp answers ENOTSUP, select /var/tmp without the rejected-candidate note.

An explicit scratch/spill directory is kept as supplied and never falls back;
explicit tmpfs/ramfs directories are now refused, including command scratch.
The explicit sink-path refusal
`requires disk backing, not tmpfs or ramfs` is unchanged. The jailed profile
still requires a sealed arena and never creates a backing file or falls back to
`TMPDIR`, `/tmp` or `/var/tmp`. File-backed command scratch uses
`scratch_io_failed` (class `resource`), including default-directory query
failures other than ENOTSUP and refusals when all directory candidates are memory-backed. Native runs create command scratch at begin,
before strategy callbacks. Report scratch uses `report_sink_failed` with
`reason: scratch_io`; auxiliary tables use `scratch_io_failed`; sink-path I/O
uses `report_sink_failed` with `reason: sink_io`.

Allocation failure is classified by its origin: `std::bad_alloc` and a required
`new (std::nothrow)` returning null raise typed `out_of_memory`, including in
scratch setup and while constructing another error's diagnostic. A prebuilt
payload allows that cause to survive status-only I/O boundaries without another
diagnostic allocation. Actual kernel `ENOMEM` from mapping, positional I/O or
disk operations retains the existing I/O code. Arena/share exhaustion and
length/extent `EFBIG` refusals retain their existing codes.

Scratch placement, in short (1.7.0 and later). Without an explicit scratch
directory a native run, with no scratch arena attached, creates one unlinked
command-scratch file at begin under the nonempty `TMPDIR`, or under `/tmp`
when `TMPDIR` is unset or empty. If that candidate is proven tmpfs/ramfs, the
engine falls back to `/var/tmp`, and only when `/var/tmp` is disk-backed. If
both candidates are memory-backed the run is refused at begin: the engine
raises `scratch_io_failed`, and the default unjailed `run_json` profile refuses
first with `run_request_invalid`; through the hub entrypoint the refusal
surfaces as container exit code 4. An explicit `PINEFORGE_COMMAND_SCRATCH_DIR`
on tmpfs/ramfs never falls back and raises `scratch_io_failed`. Example: in a
container image where `/run` is
tmpfs while `/tmp` and `/var/tmp` are on ext4, a work directory under `/run`
is memory-backed; a run with `TMPDIR` set there falls back to `/var/tmp`. To
choose the location, set `TMPDIR` to a disk-backed directory such as
`/var/tmp`; `PINEFORGE_COMMAND_SCRATCH_DIR` covers the command stores only, not
the trade ledger or the report files.

The English templates describe the texts a code is raised with; they are not
a grammar. A `{name}` marks a part that varies and need not be an argument of
that name, and one text can sit under more than one code: a frame that fails
after an inner failure was recorded (`native current execution failed`, the
configure refusal above) keeps its own text and carries that failure's code:
`run_aborted` after an abort whose report was cleared, `engine_invariant` when
nothing was recorded. A consumer reads the code and its arguments, never the
English.

## The hosted report-channel codes {#run_failure_codes_report_channel}

These seven codes have `since: 1.9.0` and `retryable: false`. They are
registered before any producer uses them. Every argument below is required
unless marked optional; the fixed English is listed in the codes table.

| Code | Closed vocabularies and measurements |
| --- | --- |
| `report_budget_exceeded` | `channel`: `json`, `aggregate`; `cap_bytes`, `used_bytes`, `requested_bytes`: integers. |
| `diagnostic_budget_exceeded` | `cap_bytes`, `used_bytes`, `requested_bytes`: integers. |
| `report_channel_failed` | `channel`: `report`, `sink`, `candidate`, `diagnostic`, `status`; `reason`: `setup`, `io`, `peer_closed`, `deadline`, `protocol`. |
| `run_isolation_unavailable` | `reason`: `platform`, `policy`, `descriptor`. |
| `run_process_refused` | `operation`: `fork`, `vfork`, `clone`, `clone3`. |
| `run_deadline_exceeded` | `stage`: `prepare`, `compile`, `run`, `finalize`, `publish`. |
| `run_producer_failed` | `reason`: `exit`, `signal`, `claim`; optional integers `exit_status`, `signal`. |

For a runtime byte-budget overflow, `used_bytes` is the accepted allowance
consumed and `requested_bytes=1` is the trusted overflow probe. The observed
lower bound is `used_bytes+1`, with no claim about unread native output. For
an admission refusal, `used_bytes=0` and `requested_bytes` is the trusted
planned representation size.

The `resource` class includes host process and transport availability and
budgets, including an unavailable producer. This classification establishes
neither blame nor refund eligibility. Existing `report_sink_failed` remains
`engine_fault`, including exhaustion; its arguments and the finalizer's
exit-8 receipt mapping are unchanged. Registration changes no existing
code, argument or exit status.

## The selected-window and report-contract codes {#run_failure_codes_selected_window}

Twenty-four rows are the failure vocabulary of the documented selected-window
wire contract (v1.3). Twenty-one are its base table: the fifteen `window_*`
codes, `report_platform_unavailable`, `report_result_invalid`,
`report_shape_request_invalid`, `report_shape_unsupported`,
`results_digest_unsupported` and `strategy_value_invalid`. The contract adds
three: `report_post_execution_failed` (the phases after the strategy has
finished), `engine_command_value_invalid` (an invalid command value the engine
derived itself) and `window_pruner_unsupported` (the optimizer's trial pruner).
Their classes, argument kinds and English are the contract's; all are
`retryable: false` and `since: 1.5.0`, and all are released. The registry
declares them and validates their arguments, which is all this page states
about them: whether a build raises one is the build's own, and a row in the
catalog is not proof that the source paths it names enforce anything.

- **Argument kinds.** `string`, `nullable_string` and `scalar` are generic
  kinds added for them (the contract's string, nullable string and scalar). An
  `integer` argument is any 64-bit integer; the wire's i53 and u53 bounds are
  enforced where the request is read, before a code is raised. The
  64-hexadecimal-character digests of `window_feed_identity_changed` are plain
  `string` arguments: the registry checks that they are text and the trusted
  wrapper that raises the code owns their shape. A vocab value may hold dots
  (`counts.window_input_bars`, `metering.billable_input_bars`).
- **`window_mode_unsupported`** takes the required `capability` and the optional
  `timeframe` and `requested`. The contract names four capabilities: for
  `selected_window_chart_timeframe` and `selected_window_request_feed_timeframe`
  `timeframe` is the canonical timeframe token refused; for
  `selected_window_request_feed_inventory` there is no `timeframe` (an
  incomplete inventory names no token, and none is invented); for
  `selected_window_preroll` `requested` is the pre-roll length. The registry
  does not tie an optional argument to a capability: the trusted wrappers that
  raise the code do.
- **`window_calendar_unsupported`** takes `session` as a `nullable_string`: a
  JSON string, or `null` when the request names no session. `window_lineage_mismatch`
  and `report_shape_request_invalid` carry request values as `scalar`.
- **Caller-only.** `window_report_mismatch` is raised by the caller that
  compares a report with its trusted window counts. `run_json` never emits it.
- **Who raises the others.** The contract's route for each code is: the caller
  or the deployed capability for `window_mode_unsupported`,
  `report_shape_unsupported` and `results_digest_unsupported`; the caller's
  request, bounds, calendar, feed or lineage for `window_request_invalid`,
  `window_range_invalid`, `window_preroll_out_of_range`,
  `window_legacy_flag_conflict`, `window_calendar_unsupported`,
  `window_boundary_unaligned`, `window_feed_range_invalid`,
  `window_no_observations`, `window_lineage_mismatch`,
  `window_feed_identity_changed` and `report_shape_request_invalid`; the
  caller's study configuration for `window_pruner_unsupported`; the
  supplied-input cap for `window_input_cap_exceeded`; the engine's window
  invariants for `window_prestart_state_violation` and `window_prestart_fill`;
  a broken image before any strategy code loads for
  `report_platform_unavailable`; a producer invariant defect for
  `report_result_invalid`; and a value the script itself supplied to a source
  sink for `strategy_value_invalid`. Two codes belong to the engine although a
  valid script can lead up to them, because reachability does not confer
  ownership: `report_post_execution_failed` (time or failure after the
  strategy has finished, in result assembly, the digest or serialization) and
  `engine_command_value_invalid` (a command value the engine derived from its
  own book, equity, margin, sizing, fee, slippage or settlement state, as
  opposed to one the script supplied).
- **Producers in this tree.** Fifteen of the 24 have a raise site here.
  The C++ source layer raises `strategy_value_invalid` for invalid actionable
  values supplied by the script; the engine kernel, native engine and Pine
  adapter raise `engine_command_value_invalid` for invalid execution values.
  The Python wrappers under `docker/` raise `window_mode_unsupported`; the
  selected-window request refusals `window_request_invalid`,
  `window_range_invalid`, `window_preroll_out_of_range`,
  `window_legacy_flag_conflict`, `window_calendar_unsupported`,
  `window_boundary_unaligned` and `window_feed_range_invalid`; the report-option
  refusals `report_shape_request_invalid`, `report_shape_unsupported` and
  `results_digest_unsupported`; `report_result_invalid`, for a non-finite
  canonical result; and `report_post_execution_failed`, for a failure after
  the strategy has finished. The other nine have none:
  `window_report_mismatch` is the caller's (above), and the producers of
  `window_no_observations`, `window_lineage_mismatch`,
  `window_feed_identity_changed`, `window_input_cap_exceeded`,
  `window_prestart_state_violation`, `window_prestart_fill`,
  `report_platform_unavailable` and `window_pruner_unsupported` are not in
  this tree. `report_shape_unsupported` and `results_digest_unsupported` are
  the typed refusals this build raises for the compact shape and the digest
  it does not declare; `report_platform_unavailable` is meaningful only in an
  image that claims the digest. `window_pruner_unsupported` is optimizer-only:
  `run_json` never raises it, because a backtest has no pruner. The contract
  has both study admissions (the Python study specification and the native
  command line) refuse a selected window combined with a pruner other than
  `none` before any trial is allocated; no raise site for it is in this tree.

## The catalog {#run_failure_codes_catalog}

`docker/run_failure_codes.json` is canonical JSON (keys sorted, one-space
indent, a final newline) with four members: `schema`, `classes`, `kinds` and
`codes`. Each code has its `class`, `retryable`, `args`
(`{name: {kind, values?, optional?}}`), its English templates `english`, the
release it first shipped in (`since`), a `description`, and, once deprecated,
`deprecated: true` and the codes that replace it (`replacedBy`).

The engine compiles two files generated from it:
`include/pineforge/run_failure_codes.hpp` (the `RunFailureCode` enum) and
`src/run_failure_registry.inc` (the registry every raised code and argument is
validated against). `scripts/check_run_failure_codes.py` regenerates them
(`--write-registry`) and, as the `source-guard-run-failure-codes` stage of
`ci_preflight` and of every `ci_verify` profile, fails when the catalog is not
canonical, a generated file is stale, a change breaks the catalog of the newest
release tag (see [Versioning](#run_failure_codes_versioning)), or a `throw` in
the Pine library or the source layer carries no code and is not on
`scripts/run_failure_throw_allowlist.txt`.

## The per-release diff and the release assets {#run_failure_codes_release}

`docker/run_failure_codes_diff.json` (schema
`pineforge-run-failure-catalog-diff/v1`) states what the catalog changed
since the newest release tag. It has the structure of the codegen's
diagnostics-catalog diff:

```json
{
  "schema": "pineforge-run-failure-catalog-diff/v1",
  "from": {"version": "1.3.0", "tag": "v1.3.0", "catalogSha256": null},
  "to": {"version": null, "unreleased": true, "catalogSha256": "<sha256>"},
  "catalogSchema": {"before": null, "after": "pineforge-run-failure-catalog/v1"},
  "metadataChanges": [],
  "added": [{"code": "...", "collection": "codes", "entry": {}}],
  "changed": [{"code": "...", "collection": "codes", "fields": []}],
  "removed": [],
  "deprecated": [{"code": "...", "replacedBy": ["..."]}]
}
```

- `from` names the release the diff starts at and the SHA-256 of that
  release's catalog file, its raw bytes; `null` when that release published
  no catalog, and then every code is `added`. `to.catalogSha256` hashes this
  tree's catalog the same way.
- A change is `{"path", "beforePresent", "afterPresent", "before", "after"}`.
  The path walks every field of an entry, dot-joined: `class`, `retryable`,
  `since`, `description`, `english`, `deprecated`, `replacedBy`,
  `args.<name>.kind`, `args.<name>.values`, `args.<name>.optional`, and
  `args.<name>` for an argument added or removed whole. The presence flags
  tell an absent field from an explicit `null`. A list is one value: a value
  added to or removed from a closed list is one change of `args.<name>.values`
  with both lists, in their declared order.
- `metadataChanges` are the same changes over the catalog's own members
  (`classes`, `kinds`); `added` and `removed` carry whole entries; `deprecated`
  lists each code deprecated since `from`, its field changes also in
  `changed`. `removed` is always empty in a release.
- The bytes are deterministic: entries sorted by code, changes by path, a
  two-space indent and one final newline, with no timestamp or path.

`scripts/gen_run_failure_catalog_diff.py` writes it (`--write`), and, as the
`source-guard-run-failure-diff` stage of `ci_preflight` and of every
`ci_verify` profile, recomputes it byte for byte (`--check`). The check also
fails when `to` is not this tree's catalog, `from.tag` is not the newest
release tag reachable from `HEAD`, a code was removed, or a code it adds names
in `since` a release that is not after `from`. A clone without tags cannot
name that tag. Where the tags are required (`PINEFORGE_REQUIRE_RELEASE_TAGS`
set to any value but `0` or empty; every workflow that runs the guards sets
it to `1`) the check then fails until they are fetched. Elsewhere, `from.tag`
must be the release `VERSION` names (the release workflow writes both in one
commit), so a diff kept from before a later release fails; a `null`
`from.catalogSha256` is taken only for a release older than 1.4.0, the first
that published a catalog; otherwise the check runs the diff backwards over
this tree's catalog (every change put back to its `before`, the added codes
dropped, the removed ones restored), requires the canonical bytes it gets to
hash to `from.catalogSha256`, and recomputes the whole diff from them. It says
so: without the tags neither `from.tag` nor `from.catalogSha256` is checked
against git, only the rebuilt catalog against the sha the diff states, and
every run with the tags checks both.

Each release attaches two or three assets beside its tarballs:

| Asset | What it is |
| --- | --- |
| `run_failure_codes-<tag>.json` | The catalog of that release, byte for byte. |
| `run_failure_codes_diff-<tag>.json` | The changes since the release the release notes start at -- for a final release, the previous final release -- stamped: `to.version` is the release's version and `to.catalogSha256` hashes the catalog asset. |
| `run_failure_codes_diff-<tag>-from-<candidate>.json` | Only on a final release that follows its candidates: the changes since the last candidate, the step in the chain of diffs (the checked-in diff, stamped). |

The release workflow stamps the diffs with `--stamp`, which fails the release
before anything is committed when the catalog changed after the diff was
written, the diff does not start at the release the notes start at or at a
release candidate after it, a code was removed, the release adds or changes
codes but is not a minor or major release (it says, in one line, to bump
minor), or a code this release adds names another release in `since` (a
release candidate `X.Y.Z-rc.N` ships the codes of `X.Y.Z`). It then commits
the diff reset to the new tag (`--reset`: from this release, nothing added)
with the version bump, and pushes that commit and the tag atomically, so the
next diff starts there. The diffs chain through every release, release
candidates included: a consumer that skips releases follows the chain of
`from.tag` values from its own release, a final release's `-from-` asset being
its step, or compares the two catalog assets. A consumer of final releases
alone reads each one's `run_failure_codes_diff-<tag>.json`, which starts at the
previous final release. Should a release fail after its tag is pushed,
`--stamp --to-tag <tag> --previous-tag <start>` rebuilds each of its diffs from
git alone, byte for byte.

## Versioning {#run_failure_codes_versioning}

- **A code is never removed, renamed or reused for another meaning.** A code
  that should no longer be raised stays in the catalog, marked `deprecated`
  with the codes that replace it in `replacedBy`.
- **A code keeps its meaning.** Its `class`, its `retryable` flag and each
  argument's `kind` do not change, a closed list never loses a value, and an
  optional argument never becomes required, unless the code is deprecated.
  Its `since` never changes.
- **Additions are additive.** A new code, a new value in a closed list, a new
  optional argument and a new English template ship in a MINOR release, the
  rule the [public contract](@ref public_contract) gives the C ABI. `since`
  names the release that first shipped a code.

`scripts/check_run_failure_codes.py` holds these rules against the catalog of
the newest release tag reachable from `HEAD` (in a clone without tags, the
catalog the checked-in diff rebuilds, as above). The diff's own checks hold
the rest: a code added since a release names a later one in `since`, and the
release workflow's `--stamp` refuses a release that adds or changes codes
unless it is a minor or major release over the release its diff starts at
(after a candidate of its own version, unless that version is an `X.Y.0`).

## Generated code {#run_failure_codes_codegen}

A strategy the transpiler generates names its stops' codes through
`<pineforge/run_failure.hpp>`, under `#ifdef
PINEFORGE_HAS_RUN_FAILURE_CODES_V1`. Each helper is `[[noreturn]]`, throws
`coded<std::runtime_error>`, hardwires its code and takes only literals the
transpiler emits; the English it is given is what #strategy_get_last_error
reads.

| Helper | Code |
| --- | --- |
| `pine_runtime_error(message)` (`<pineforge/log.hpp>`) | `strategy_runtime_error` |
| `pine_no_data_stop(function, call, line, english)` | `no_data_request` |
| `pine_other_symbol_stop(function, symbol_literal_or_null, call, line, english)` | `other_symbol_request` |
| `pine_array_stop(reason, method_or_null, english)` | `pine_array_error` |
| `pine_collection_stop(collection, reason, english)` | `pine_array_error` (`historical_modified`) or `pine_na_reference` (`na_reference`); `collection` is `array` or `matrix` |
| `pine_na_stop(object, english)` | `pine_na_reference` |
| `pine_limit_stop(limit, max, english)` | `pine_runtime_limit` |
| `pine_unsupported_stop(reason, line, english)` | `request_unsupported` |
| `pine_string_stop(reason, english)` | `pine_string_error` |
| `pine_engine_invariant(english)` | `engine_invariant` |

The generated wrapper records an exception that escapes the strategy with
`note_run_failure(engine, entrypoint, error)`: the text it writes is still
`"<entrypoint>: <what()>"`, and the code is the exception's own.
`note_run_failure_unknown(engine, entrypoint)` records a non-standard
exception as `engine_unclassified_error`.

A legacy setter the strategy latched is recorded with
`note_run_failure(engine, text, code, args)` or, with the value it already
holds, `note_run_failure(engine, text, value)`. Its exception type derives
from `checked_settings::LatchedSettingsFailure` and from `RunFailureInfo`,
which it also holds as a copy-assignable member, built with the
`RunFailureInfo(code, args)` constructor and read with `run_failure()`. Those
names, `RunFailureValue`, and the codes `RunFailureCode::none`,
`setting_rejected` and `out_of_memory` are part of the contract too. Where the
script is prepared, an exception that already carries a code keeps it; a
latched failure without one reads `setting_rejected`.

## Strategy identifier admission

The Pine source host's command admission, including selected-window replay,
refuses IDs, entry selectors and OCA names containing NUL (0x00) or unit
separator (0x1f) before they can enter the adapter's composite keys. After
UTF-8 validation, this raises `pine_invalid_argument` (class `strategy`) with
`rule=unsupported_combination`, `function` naming the strategy command and
`argument` naming its refused operand. For example, an exit ID carries
`{"argument":"id","function":"strategy.exit","rule":"unsupported_combination"}`.
The check uses the string's length, so an embedded NUL is refused too.
Spaces, quotes, punctuation and other valid UTF-8 bytes remain accepted.
Comments retain their existing UTF-8 admission, including these two bytes.
Invalid UTF-8 still raises `strategy_value_invalid` with
`reason=invalid_unicode`.

Two live order keys whose 64-bit hashes collide are refused with
`engine_invariant` and `{"reason":"order_key_hash_collision"}`. The optional
`reason` is a closed vocabulary with only `order_key_hash_collision`;
other invariant failures keep `{}`. The English remains
`Pine live source key collision`.

## Finite run quota contract {#run_failure_codes_quota}

The quota service and its native and Python clients share one attempt.
`PF_RUN_SCRATCH_BYTES`, `PF_RUN_SCRATCH_FILES`, `PF_RUN_OUTPUT_BYTES` and
`PF_RUN_DESCRIPTOR_LIMIT` are host inputs parsed once as ASCII decimal.
Unset means `missing_limit`, malformed means `malformed_limit`, and values
above 9007199254740991 mean `counter_overflow`. Explicit zero admits zero
capacity. These helpers define admission; their presence does not establish
that every producer or launcher in a build uses them.

`run_resource_quota_exceeded` and `run_descriptor_limit_exceeded` are terminal
resource failures with `retryable: false`. Their snapshot is taken before
the denied reservation: `used = committed + reserved`, `requested` is the
extra capacity, and `peak` includes earlier pending reservations. The receipt
keeps `committed` and `reserved` separately. Bytes, files, output and
descriptors are checked in that order before adding any of the deltas.
Partial realization remains charged; cancel returns only the unrealized
portion. Successful shrink or last-close releases storage; already produced
output stays charged for the whole attempt, including repetitions.

The required `stage` vocabulary is `admission`, `transpile`, `compile`,
`execution`, `assembly`, `finalizer`, `converter`, `publication`. Resource
artifacts are `workdir`, `compiler`, `table`, `ledger`, `arena`, `curve`,
`sink`, `spool`, `column_index`, `column_data`, `json`, `stdout`, `conversion`,
`diagnostic`. Descriptor `process_role` is `host`, `compiler`, `worker`,
`finalizer`, `converter`, `reaper`; `soft_nofile` is the host-recorded soft
process limit. All reported counters are nonnegative integers at most
9007199254740991.

`run_quota_configuration_invalid` is a terminal input failure. Its required
`reason` is `missing_limit`, `malformed_limit`, `counter_overflow`, `protocol`,
`containment_unavailable`, `unmanaged_path`, `unknown_bound`, `unsupported_route`.
Its required `setting` is `scratch_bytes`, `scratch_files`, `output_bytes`,
`descriptors`, `compiler`, `facts`, `workdir`, `channel`, `profile`, `protocol`.
A missing authenticated channel forbids growth. Tickets retain their original
attempt; teardown and a later attempt cannot rewrite a refusal or refund
capacity into a different owner. Backing identity requires host confirmation
from a descriptor before the ledger accepts a file's realization.
