R19–R22 implementation diff prepared; NOT verified or ready to declare complete.
No compiler, build, test, repository script, installation, staging, commit or
push ran on this Mac. Only reads, git inspection and file edits were performed.
`git diff --check` passed. No lab connection was exposed/provided, so there are
no RED/GREEN receipts, measured allocation totals, ctest summaries or log paths.

HEAD/base: `583c2217d2524e8f2bc4c8a77dfa336309d741fc`.
Branch: `o1w/pyjson`. The supervisor must commit the working-tree diff.
`git diff --stat 583c2217d2..HEAD` is empty because this lane made no commits.
The tracked working diff is 3 files, +730/-386; four new files (including this
report) are untracked and must be included in the supervisor's commit.

Two dependencies remain:

1. `_ReportSink.verify()` and `_ReportSink.records()` are outside the assigned
   function ranges and still instantiate the buffered `SinkReader`. Both must
   use `_open_uncached_sink(self.path)` instead of `SinkReader(self.path)`.
   A replacement factory and an independent test are provided, but the factory
   is NOT connected to these methods without the requested scope
   extension. Until both are connected before the first full scan, sink
   readback can populate cache proportional to output length. Do not claim
   an end-to-end charged-memory bound for R19/R21/R22.
2. The contract lane's `docs/design/o1-report-contract.md`, revision 1 with
   round-2 reader declarations, now exists and was read. Its implemented
   provider/ctypes bridge is absent on this base. No SUMMARY_ONLY support,
   replay, empty-array inference, new flag or new export is advertised here.
   The native provider must supply the frozen JSON summary folds/logical
   counts; transport negotiation must select it at the ordinary and selected
   builder-selection sites. Parser/provider edits are outside this lane.

| Row | Before at base | Diff and resident bound | Intended fail-before evidence; all unexecuted |
| --- | --- | --- | --- |
| R19 | `src/report_sink_writer.cpp`: header/descriptor strings grew while formatting; records constructed owning strings; ordinary buffered writes left charged data cache behind. | One 1 MiB `Line` scratch in the writer state, one 64 KiB aligned I/O window plus 4095 alignment bytes, fixed numeric/UTF-8 token scratch and existing configuration strings. Header and all E/T/O/V/K/X/H records format into that scratch without per-record owning strings. Checks precede copies and retain the existing 1 MiB wire limit. Linux O_DIRECT, padding/truncation of the final disk block and fdatasync per window prevent accumulating buffered data windows. Public `f64`/`escaped_string` keep their owning APIs but are not called by the writer. | `test_report_sink_writer_memory`: 8 versus 256 long X records, verified receipt and physical line populations; prints calls, cumulative allocated bytes, peak retained bytes and cached pages separately. Base allocation traffic grows with records and base output pages remain cached. Existing `test_report_sink_format` supplies the independent million-value formatter/UTF-8/header/trade/trailer byte oracle. |
| R20 | `build_report_dict` at base line 3810 retained T trade dictionaries, T PnLs and Bs curve dictionaries; selected and ordinary callers retained T incarnations. Ordinary and selected sink folds/iterators were already present in batch-B. | Scalar wins/losses/extremes and the exact `cum += pnl; peak=max(peak,cum); max_dd=min(max_dd,cum-peak)` trade-PnL fold. First PnL seeds best/worst, including signed-zero ties. Repeatable row/point iterators retain one record and support the selected curve's length/anchor checks. Read-only incarnation accessors get the old refusal pre-pass and are revisited without storing values. Native owning report arrays remain outside this Python transport change. | `test_python_trade_curve_and_serialization_peak_is_flat`, `test_legacy_main_assembly_peak_is_flat`, `test_selected_legacy_main_assembly_peak_is_flat`: T=256/24000, actual Bs/native feed populations asserted, tracing ends before the test consumer retains stdout. Base dictionaries/PnLs/curve cause MiB-scale growth. Frozen array-byte comparisons include signed zero and cancellation-sensitive PnLs. |
| R21 | Selected base lines 4718–4722 and ordinary 5227–5231 used StringIO without a native sink. Batch-B already prepared sink-route JSON in a file, but buffered file I/O did not bound charged cache. | All report routes prepare an unpublished artifact through the streaming serializer, synchronize it, perform native cleanup, then publish with the existing stdout phase/error rules. Each file has one 64 KiB aligned anonymous working window; publication reads at most 1 MiB. No whole-report encoding string. String interiors encode in 4096-character pieces. Circular-document ValueError and legacy versus sink error classification are preserved. | The main-route peak tests and `test_legacy_routes_never_use_a_stringio_report` catch the old StringIO route. `test_artifact_cache_is_bounded_on_the_deployment_backing` writes/reads 2 MiB and 24 MiB plus a tail, checks actual byte populations and uses mincore to require at most one cached tail page; buffered base files fail. Existing failure tests were moved to the real field encoder/artifact write sites. |
| R22 | `make_outputs_reader` copied whole native arrays and accumulated events; old-module/selected callers invoked the array builder. Batch-B already externalized sink output columns, but their buffered temps accumulated cache. | Native output copies use at most 4096 values; events yield individually. Borrowed event messages decode UTF-8 with replacement in fixed 4096-byte pieces, avoiding a whole c_char_p copy. Old-module outputs stream in all caller routes; a bounded validation walk keeps their reader failures in assembly. Sink columns use the uncached artifact and a 64 KiB serializer copy window. S manifest-defined series require S+3 fixed column windows/files; constants/manifests remain per-definition metadata. Column order/rgba-u32/null/UTF-8 encodings stay unchanged. | `test_legacy_main_assembly_peak_is_flat[True]` asserts actual T, Bs, two series, E and max native copy size; unchanged main/reader materializes them. `test_columns_do_not_accumulate_python_objects` catches the old 1 MiB column-copy peak, while the cache test catches dirty data pages. `test_borrowed_utf8_is_encoded_in_fixed_chunks` compares with the independent whole decode/JSON encoder. Ordinary/selected frozen byte tests cover output columns/events. |

Backing contract: the qualification target is Linux on a real disk filesystem
with effective O_DIRECT and 4096-byte-compatible direct I/O, synchronous
pread/pwrite and fdatasync. `TMPDIR`, `--spill-dir` and named sink paths must
use that backing. tmpfs/ramfs/memfd are rejected; there is no buffered fallback
and no advisory-eviction claim. The Darwin F_NOCACHE branch is retained for
compatibility but has no measured qualification. The new mincore tests inspect
data-page residency rather than infer it from a syscall's success. Full cgroup
charging, filesystem metadata, W=1/W=4 and deployment filesystem behavior still
need the lab gate; mincore alone is not that gate.

Batch-B already satisfied the ordinary sink summary's scalar PnL folds,
ordinary/selected row iterators, selected sink JSON preparation and external
sink output-column layout. This diff extends those pieces; it does not redo
the native report APIs, selected-window refusals, metric replay or trade storage.

Touched files:

- `docker/run_json.py`
- `src/report_sink_writer.cpp`
- `docker/run_json_bounded_transport_test.py` (new)
- `tests/test_report_sink_writer_memory.cpp` (new)
- `tests/test_run_json_post_execution.py`
- `tests/cr150.d/core-cr150-o1w-pyjson.cmake` (new)
- `REPORT.md` (new handoff)

No existing public documentation anchors, ABI declarations/counts, report
schema fields or fingerprint versions changed. No execution hash leaf or
broker durable state changed. New state belongs to output formatting/transport.

Lab work still required, on the exact committed tree:

- Compile the new C++ test first against the frozen previous header closure;
  run the new tests on the unchanged base with tests only, record actual first
  diagnostics/runtime RED reasons, then run them with the implementation.
- CTest: `test_report_sink_writer_memory`, `test_run_json_bounded_transport`,
  `test_report_sink_format`, `test_report_sink_l6b3`,
  `test_run_json_sink_l9a`, `test_run_json_selected_sink_l9s`,
  `test_run_json_sink_summary_l9b_native`,
  `test_run_json_selected_sink_native_l9s`,
  `test_run_json_selected_sink_memory_l9s`,
  `test_run_json_post_execution`, `test_run_json_nonfinite_report`,
  `test_cr150_cmake_d`.
- Run full preflight, release and debug verification. The supervisor owns
  corpus/parity and whole-cgroup W=1/W=4 qualification. No campaign sweep was run.
- Compare ordinary, jailed entrypoint and selected outputs on real populated
  modules byte for byte. The jailed launcher is not a separate producer in
  this repository; its actual deployment execution remains unverified.

Reserve these functions/files against other lane edits: the two Python
report builders' assembly/serialization call sites, `make_outputs_reader`,
`build_report_dict`, `_JSONRecordArray`, `_JSONUTF8`, `_BoundedJSONFile`,
`_UncachedSinkFile`, `_open_uncached_sink`, `_JSONColumn`, `_json_temporary`,
`_stream_json`, `_stream_json_value`, `_prepare_stream_report`,
`_legacy_streamed_outputs_block`, and all of `src/report_sink_writer.cpp`.
Coordinate the pending two-method sink-reader change explicitly; selected
reader refusals and other native report functions were not edited.
