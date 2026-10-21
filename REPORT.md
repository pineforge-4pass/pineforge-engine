Round 5 R21/R22 diff prepared: both sink methods use the bounded reader, JSON
temporaries have an unbuffered binary owner, failed serialization closes its
unpublished artifact immediately, and the lab driver is supplied. The remaining
StringIO is the failure diagnostic formatter, documented below. These are source
changes and authored regression rows, not a verification or performance verdict.

HEAD/base: `136a7b68a6139af72072cc7e89d276fa36ca7d1a`, branch `o1w/pyjson`.
Initial `git status --porcelain=v1` stdout was empty. Round 1 is already committed
at this HEAD; its historical report is available with `git show 136a7b68a6:REPORT.md`.
This round leaves an unstaged diff and one new driver for the supervisor to commit.
No staging, commit or push ran, following the explicit edit-only lane instruction
and COMMON.md. `git diff --stat 136a7b68a6..HEAD` is therefore empty.

The three gaps:

| Gap / rows | Before at 136a7b68a6 | After in working tree | Observable and authored test row |
| --- | --- | --- | --- |
| 1 / R21, R22 sink readback | `docker/run_json.py:5401` (`_ReportSink.verify`) and `:5422` (`records`) created buffered `SinkReader` instances directly. | `docker/run_json.py:5403` and `:5424` call `_open_uncached_sink`. Its existing strict parser, receipt checks, error wrappers and full exhaustion checks stay in place. | Header/counts/SHA, immutable records, physical ordering, every tag filter, repeated scans and the last partial I/O window must agree with `pf_sink.SinkReader`. `test_report_sink_methods_use_uncached_reader_and_preserve_order` uses 5003 E, 3 T, 1 O, 1 V (message spanning a window), 1 K; asserts the factory is actually used and compares all records. `test_report_sink_reader_errors_match_buffered_reader` compares exact exception type/code/text for bad SHA, missing trailer, extra bytes and missing final LF, through both methods. `test_report_sink_changed_after_verification_consumes_the_tail` pins late digest refusal, yielded counts/tail values, and temporary versus caller-owned cleanup. |
| 2 / R21 JSON artifact and R22 column owner | `docker/run_json.py:5716` opened a text `TemporaryFile(mode="w+", encoding="ascii", newline="\n")`. Round 1 already wrapped that owner in `_BoundedJSONFile`; serialization/readback itself already used its aligned direct I/O. `:5809` registered failed artifacts with the caller's longer-lived stack. | `docker/run_json.py:5720` opens `TemporaryFile(mode="w+b", buffering=0)` beneath that same bounded writer. `:5812` scopes preparation with a local ExitStack, transferring ownership only after serialization, LF, flush and rewind succeed. | Compact ASCII JSON bytes, escaping, key/row order, signed zero, final LF, phase/error classification and publication after native cleanup must stay identical. `test_array_transport_is_the_frozen_bytes` now includes 517 trades/1034 curve points. `test_json_temporary_is_unbuffered_and_preserves_multichunk_bytes` checks a real FileIO owner and every byte beyond 1 MiB against json.dumps. `test_failed_serialization_closes_unpublished_artifact_immediately` injects OSError/MemoryError/ValueError/KeyboardInterrupt after a real window write with the outer stack still open; checks closed fd/window and preserved user file. `test_automatic_sink_midwrite_failure_publishes_only_failure` executes real main assembly/serialization, then checks its sole failure line, closed artifact and removed temporary sink. Existing `test_persistent_temporary_io_error_keeps_the_disk_code` now injects at the binary owner and still pins actual flush/close failures. |
| 3 / R20 report-size buffer audit | `docker/run_json.py:2197` is `_dump_line`'s StringIO. | `docker/run_json.py:2199` retains it; `:2197` documents its callers. This is the `failure_line -> _dump_line` failure-diagnostic route, outside the ordinary/selected successful per-run report producers. The sole production caller is failure_line; no trade, curve or output array is passed to it. | Failure text is clipped to 16 KiB UTF-8, each string argument to 1 KiB, and the final diagnostic envelope to 60 KiB plus LF. The scratch can exceed the final envelope while testing overflowing arguments, but it does not scale with report rows. `test_failure_only_stringio_keeps_frozen_bytes` compares full bytes with the immutable pre-L9A producer, including surrogates, Unicode and argument overflow. `test_automatic_report_never_calls_failure_stringio` forbids this helper during an actual populated automatic-sink report and checks real trade/curve counts. Existing cap rows in `docker/run_json_codes_test.py` also pin the diagnostic contract. |

Expected fail-before reasons, NOT executed: on this round's base the sink-method
factory witness sees zero opens; the JSON owner is TextIOWrapper rather than
FileIO; and an artifact remains open after a caught serialization error until
the outer stack exits. Byte/order/error rows are compatibility controls and
should also pass before these fixes. No runtime RED/GREEN receipt is claimed.

The resident bound is the existing 64 KiB aligned I/O window per reader/artifact
or output column, at most 1 MiB per publication read, and the strict reader's
existing 1 MiB line/header bound and per-definition descriptors. No report rows
are collected by the production transport. Qualification still requires a real
Linux disk filesystem with effective O_DIRECT and compatible 4096-byte I/O;
tmpfs/ramfs/memfd are rejected. Darwin F_NOCACHE remains unqualified. This round
does not change the direct-I/O writer's synchronization policy or prove cgroup
charging. The existing cache and Python peak rows remain lab work.

The new driver is `scripts/measure_run_json_temporary_sink.py`.
From the repository root, SM-CX uses this one-line command, with the environment
variables naming the same CSV/module and the recorded report's elapsed value
and raw SHA-256 (disk-backed OUT_DIR):

```bash
python3 scripts/measure_run_json_temporary_sink.py "$CSV_1M" o1w-pyjson-r5 "$OUT_DIR" --so "$STRATEGY_SO" --expect-bars 1000000 --elapsed-seconds "$PRECHANGE_REPORT_ELAPSED_SECONDS" --expected-sha256 "$PRECHANGE_REPORT_SHA256"
```

The driver runs the chosen producer's actual `run_json.main` once in a fresh process.
Its observer forwards the real sink factory, requires one automatic temporary
sink, refuses an unavailable sink ABI instead of measuring a legacy fallback,
and checks removal after native cleanup. `wall_s` uses perf_counter across that
process; `peak_rss_mb` uses wait4's per-child high-water mark (MiB). The small
receipt sidecar is included in the child lifetime. After the measured process
exits, the parent scans the complete CSV and streams the unmodified report in
64 KiB windows to count its actual trade/equity items and compute raw SHA-256.
Counts must match the verified sink receipt and the CSV/report input counts.
There is no sampling or truncation.

Success prints exactly one JSON line with `wall_s`, `peak_rss_mb`,
`report_bytes`, `trades`, `equity`, and `sha256`; the complete raw report is
published as `$OUT_DIR/o1w-pyjson-r5.report.json`. A hard link publishes only the
finished file and refuses to overwrite an existing caller file. Failures return
nonzero, print the failure on stderr, remove private scratch files, and publish
no report. The label is only an artifact name; it is not injected into the report.

`--elapsed-seconds` fixes only the producer's report-duration clock to the
recorded value; wall/RSS measurements remain real. This is needed for raw
byte comparison because elapsed_seconds otherwise varies on every run.
The census only reads the report; it never rewrites or re-encodes it. `--producer` can point
at the pre-change run_json.py with its own helper closure for a fresh comparison.
If recording a new baseline, use the same module, CSV path, settings and elapsed
value on both producers. Optional settings include input/script timeframe,
magnifier switch, inputs, overrides, generated.cpp, syminfo and outputs.

Driver rows are in the existing `docker/run_json_bounded_transport_test.py`:
`test_driver_census_counts_real_arrays_and_hashes_all_bytes` compares 517/1034
populations, escaped strings across window boundaries and the independent raw
digest; the truncation/extra-document row refuses incomplete output.
`test_driver_measures_real_main_once_and_preserves_published_bytes` runs the
real producer in a child with only native exports substituted, requires positive
measured wall/RSS, checks all 23 input/curve rows and 3 trades, then compares
published bytes with a direct main invocation. The failure row exercises a
population mismatch, digest mismatch, real sink refusal and absent sink ABI;
the ownership row preserves an existing caller report. The parser row checks
help, missing positional input and missing input file without invoking a producer.
All rows are authored and unexecuted.

No compiler, py_compile, test, preflight, build, corpus sweep, measurement,
installation or lab connection ran. `git diff --check` passed; source review
also found and updated the existing ordinary sink test's text-mode fault hook
so it continues to inject at the new binary owner. There are no verification
summary lines, lab log paths, measured numbers or speed claims.

Lab checks still required: py_compile of the producer/driver and changed tests;
the driver help and missing-input invocations; `test_run_json_bounded_transport`,
`test_run_json_sink_l9a`, `test_run_json_selected_sink_l9s`,
`test_run_json_post_execution`, `test_run_json_nonfinite_report`, then the full
required C++/corpus and maintainer verification/parity gates on the committed
tree. Do not treat this diff as release proof.

Touched files: `docker/run_json.py`, `docker/run_json_bounded_transport_test.py`,
`tests/test_run_json_sink.py`, the new driver, and this report.
Neither pending-ruling test file was edited. No C++/native measured source,
public ABI, schema field, fingerprint version, durable hash leaf, documentation
anchor/count, dependency or CMake registration changed.

Already present before this round: batch-B's streaming sink serializer and
external output columns; round 1's bounded JSON artifact/column writer, uncached
reader factory, scalar Python trade folds and native output iterators. Those
implementations were extended, not replaced. R19 has no new change this round.
The separate SUMMARY_ONLY/provider contract dependency from round 1 is not
resolved or advertised by these three transport fixes.

Open lab input: the original campaign 1M driver's path, native module/settings,
recorded duration and SHA were not supplied or found in this checkout. The
runner is placed with the repository's Python drivers under scripts; the
supervisor must bind the command's variables to those existing campaign inputs.
No claim is made that the historical time difference was caused by the residual
text owner, or that changing it produces any particular speed.

Reserve `_ReportSink.verify`, `_ReportSink.records`, `_json_temporary`,
`_prepare_stream_report`, the round-1 transport helpers, the new driver, and the
changed regression rows against concurrent lane edits until the supervisor
collects this diff. Suggested supervisor commit message:
`CR-150 o1w-pyjson: wire sink readers, unbuffer JSON temps, audit failure StringIO; add measurement driver`.
