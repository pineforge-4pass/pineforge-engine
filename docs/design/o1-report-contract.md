# O1 report contract, revision 1

Status: native C++ runtime and consumer contract for 1.7.0. Mode selection
and envelope reads are implemented in `src/engine_report.cpp`. Exact bounded
SUMMARY_ONLY still requires an eligible host and a provider for any required
second pass; the engine does not schedule that replay. Transport negotiation
and remote verification remain separate acceptance gates.

## Versions and entry points

The versioned report envelope is `pineforge-report/v1`. Its C data declarations
live in `<pineforge/report_mode.h>`; the native C++ declarations are
`ReportMode`, `ReportEnvelopeV1`, `BacktestEngine::set_report_mode_v1` and
`BacktestEngine::fill_report_envelope_v1` in `<pineforge/engine.hpp>`.
The native methods implement mode admission and report assembly, subject to
the eligibility and completion requirements below.

The single C++ reader/visitor family lives in `<pineforge/report_pages.hpp>`,
which includes `report_mode.h`; `engine.hpp` includes both. Its namespace is
`pineforge::report`. It adds no mode enum, envelope version, feature probe or
C export. The completed-row and presented-point provider contracts share the
ledger and equity-metric boundaries. Reader declarations alone do not
establish provider availability.

The envelope contains `metadata` followed by `report`, the unchanged ABI-4
`ReportC` / `pf_report_t` payload. Do not append fields to either legacy struct:
old callers allocate that exact size. `PF_ABI_VERSION` remains 4; envelope
version 1 is an independent, opt-in contract. No existing function can be
passed the address of an envelope in place of a report.

Native clients select a mode before execution, check that the setter returns
`PF_REPORT_MODE_ACCEPTED`, run normally, then read the envelope. They initialize
`metadata.struct_size = sizeof(pf_report_metadata_v1)` and `metadata.version = 1`
before the read. A bad descriptor leaves the destination unchanged. A valid
read returns an independently owned report payload, freed exactly once with
`BacktestEngine::free_report(&envelope.report)`. It never re-executes a script,
invokes presentation/observer callbacks again or emits another sink record.
An envelope must not already own an unfreed payload when filled.

The C-compatible structs do not themselves add a C export. The initial native
host integration can link the C++ methods. A C/ctypes loader must negotiate a
provider implementing this contract before selecting SUMMARY_ONLY; ABI 4,
sink-v1 support or the presence of this header is insufficient. A provider
bridge for that loader must be landed with its implementation and inventory
updates, not advertised by an unimplemented runtime symbol.

Transport spelling is `--report-mode FULL|SUMMARY_ONLY|STREAMED`. No option
keeps the legacy route. An explicit request must either return the requested
versioned envelope or fail. Missing provider support, including an old module,
is the structured failure `report_mode_unsupported` with
`{"mode":"SUMMARY_ONLY","contract":"pineforge-report/v1"}` for that request.
The diagnostic must contain `unsupported report mode: SUMMARY_ONLY`. Refuse
before execution; never retry with FULL, drop the option or infer support
from an empty legacy array. This is a transport negotiation error, not a new
broker failure code.

For the SM-CX HPO host, after negotiating an implemented provider, the first
native contract call is
`engine.set_report_mode_v1(pineforge::ReportMode::SUMMARY_ONLY)` before
begin/run. Require `PF_REPORT_MODE_ACCEPTED`; any refusal ends that request.
After execution initialize/read `ReportEnvelopeV1`, require its accepted
read status and `metadata.complete==1`, then consume `report.metrics` and
logical counts. The HPO summary path does not acquire captured report readers.

Setter/read status words are:

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `PF_REPORT_MODE_ACCEPTED` | The operation was accepted. This alone does not certify completion. |
| -1 | `PF_REPORT_MODE_INVALID` | Invalid descriptor, version, mode word or destination. |
| -2 | `PF_REPORT_MODE_UNSUPPORTED` | This provider does not implement the requested mode. |
| -3 | `PF_REPORT_MODE_BUSY` | Execution, dispatch or a live stream prevents reconfiguration/read. |
| -4 | `PF_REPORT_MODE_CONFLICT` | Requested capture conflicts with the configured sink/presenter. |
| -5 | `PF_REPORT_MODE_REPLAY_UNAVAILABLE` | SUMMARY_ONLY cannot reproduce this input/host/presentation exactly. |

Rejected configuration is atomic and leaves the previous configuration
unchanged. A successful setter applies to subsequent runs on that handle,
until replaced while idle. It does not reinterpret a previously frozen report.
Unknown values are invalid, never aliases for FULL. A provider with declarations
but no implementation must not be reported as supporting any new mode.

## Modes

* **FULL (0):** today's owning-array report behavior, including metric
  definitions, trade ordering, terminal presentation and selected-window
  semantics. Existing calls and legacy sink behavior remain unchanged.
  Explicit FULL through the new envelope requires the owning-array route;
  an installed external sink is a conflict. This compatibility mode has
  O(B + T + E + D) result storage and makes no fixed-memory claim.
* **SUMMARY_ONLY (1):** the HPO trial. Return every metric block and summary
  scalar with the same bits as FULL on the same completed execution, once
  exact presentation/replay eligibility is established. The current
  `set_report_mode_v1` selects the returned shape: it omits owning report
  arrays, but does not change execution-time equity or optional-sequence
  retention. That setter alone does not establish fixed-memory recording.
  A bounded summary provider must also select
  `set_equity_report_mode(ReportMode::SUMMARY_ONLY)` and
  `set_optional_report_mode(report_sequence::ReportMode::SummaryOnly)`
  before begin, and supply the exact replay described below. The equity
  control keeps a held point and fixed folds; the optional-sequence control
  omits trace/name, hash and output row/event histories. The legacy equity
  control refuses SUMMARY_ONLY with `PF_REPORT_MODE_REPLAY_UNAVAILABLE`
  (`-5`) while an execution observer is registered. Registering an observer
  while legacy SUMMARY_ONLY is pending returns `-2`; switching the pending
  mode to FULL permits registration regardless of a past run's active mode.
  A quiescent null-observer clear remains accepted. Selected-window capture
  with legacy SUMMARY_ONLY fails closed with a typed run failure (no report
  is published): `engine_invariant` when its report view is unavailable, or
  `report_mode_unsupported` at begin where the begin refusal is present.
  The fixed-memory
  target folds finalized rows/points in order without retaining report
  sequences in RAM or a private curve tape; no curve file, tmpfs, memfd or
  allocate-then-free array qualifies.
  Exact internal historical trade reads required by a script remain the
  ledger lane's responsibility; they are not report capture. A configured
  sequence sink conflicts with this mode. Recording/trace switches remain
  independent of the returned report shape; configure their retention
  explicitly rather than inferring it from an empty returned array.
* **STREAMED (2):** return the same metrics/scalars while emitting complete
  requested sequences to an external, bounded-buffer sink. Require an
  installed `pf_report_sink_v1`; do not silently fall back to owning arrays.
  Backpressure must bound resident buffers, without truncation or a new
  population-dependent capacity refusal. Output length may grow; resident
  pages, formatting scratch, indexes and charged I/O windows may not.
  Completion requires a validated complete sink receipt.

File-backed scratch for ordinary runs requires disk backing even when no sink or scratch directory was configured.
The default selector first checks that the candidate is a searchable directory, then checks the filesystem type with `statfs`:
use nonempty `TMPDIR` if disk-backed; when `TMPDIR` is unset or empty, retain disk-backed `/tmp` as the legacy first candidate.
If that candidate is proven tmpfs/ramfs, try `/var/tmp`, also only if disk-backed. A failed filesystem query stops default resolution
with its syscall errno and names the failed candidate at every create wrapper; it never permits fallback, except as described below.
Directory names never determine the medium, and tmpfs/ramfs is never silently selected. Once a disk-backed candidate is selected,
ordinary creation/write failures keep their existing failure kinds and errno. Every create wrapper retains its established message prefix
and appends the directory-selection detail. If creation in the selected `/var/tmp` fails, the detail names that directory, the rejected
first candidate and `set TMPDIR to a disk-backed directory`, except as described below.

An ENOTSUP probe gives no filesystem evidence: use the default candidate as in 1.6.3 (error 0, no tmpfs claim, no refusal note).
This covers hosts with no probe (neither Linux nor macOS) and real ENOTSUP from stat/faccessat/statfs; the test is by errno value,
so on Linux ENOTSUP and EOPNOTSUPP are both 95. A FUSE/9p/network mount answering 95 keeps its candidate; a later create failure
keeps the plain 1.6.3 text. If the first candidate is proven tmpfs/ramfs and /var/tmp answers ENOTSUP, select /var/tmp without the rejected-candidate note.

If both default candidates are proven tmpfs/ramfs, refuse with `ENODEV` and
`scratch requires disk backing`. The failure detail names every candidate tried
and its rejection reason, followed by `set TMPDIR to a disk-backed directory`.
An explicitly configured directory is kept as supplied, never redirected to a
default; explicit tmpfs/ramfs scratch is now refused, including command scratch.
An explicit sink path keeps its existing
`requires disk backing, not tmpfs or ramfs` refusal.

The jailed profile is unchanged: it requires a sealed arena and never creates
a backing file or falls back to `TMPDIR`, `/tmp` or `/var/tmp`. Default directory
selection does not permit report capture in SUMMARY_ONLY; its no-curve-file
rule above still applies.

Each sequence retains its existing report order: closed trades followed by
report-only range-end rows, presented equity points including a selected
anchor, broker hashes at their original cadence, trace/name association, and
recorded output row/event order including rollback rules. Do not sort again
or publish a still-amendable row. Preserve sink-v1 interleaving and encodings;
this contract introduces no second sink file format.

Summary reports may keep metadata bounded by the immutable script definition,
such as per-security-site counters, manifests and run constants. No metadata
table may grow with observed bars, trades, events or distinct dynamic keys.
Dynamic trace names in STREAMED require the sequences lane's exact external
dictionary/lifetime solution; an unbounded resident intern table is not
covered by calling it metadata.

## Envelope fields and sequence capture

`pf_report_metadata_v1` is a fixed 128-byte header. `struct_size` is 128,
`version` is 1, `mode` is the accepted numeric mode, and `complete` is 0 or 1.
`complete == 1` means the current attempt completed, report validation
succeeded, SUMMARY_ONLY replay witnesses matched when required, and STREAMED
publication completed when required. Empty results, nonzero counts and setter
success cannot substitute for this flag and the run's existing status/error.
A refused attempt must not expose the preceding attempt as complete.

Each of `trades`, `equity_curve`, `broker_state_hash`, `trace`, `trace_names`,
`output_rows` and `output_events` is a 16-byte `pf_report_sequence_v1`:

| Field | Meaning |
| --- | --- |
| `logical_count` (`int64_t`) | Number of final logical records produced by the authoritative pass, independent of capture and resident length. Disabled optional sources produce zero. The one exception is `trace_names`: its `logical_count` is the number of names RETAINED in the name table (a name dictionary would grow with distinct dynamic names, against the bounded-memory rule), so it is 0 when the sequence is not captured (the optional summary control and STREAMED) and N in FULL. |
| `captured` (`uint32_t`) | Explicit 0/1 flag: the sequence was captured as requested output. A successfully captured empty sequence has 1; a deliberately omitted sequence has 0. |
| `external` (`uint32_t`) | Explicit 0/1 flag: captured output belongs to the external sink. Implies `captured == 1`. It does not transfer sink ownership to the report. |

FULL has `captured=1, external=0` for enabled sequences it retains, even when
empty. A sequence omitted by
`set_equity_report_mode(ReportMode::SUMMARY_ONLY)` or
`set_optional_report_mode(report_sequence::ReportMode::SummaryOnly)` instead
has `captured=0, external=0` in FULL; its logical count is unchanged.
SUMMARY_ONLY has `captured=0, external=0` for every sequence, even when its
logical count is nonzero. STREAMED has `captured=1, external=1` for enabled
sequences, including those whose resident history is omitted, because the
sink still emits their records. Disabled optional sequences have both flags
zero. Trades are always enabled. Equity is enabled when a report-point
provider exists; a completed bare HostRecorded run can still have no kernel
points. SUMMARY_ONLY requires
an exact point/presentation provider for the metrics FULL would compute.
Flags describe capture policy on an incomplete report too; only `complete`
certifies that the entire sequence or summary is valid.

`metadata.equity_curve.logical_count` is the logical point count;
`report.equity_curve_len` is only the resident array length. For completed
ordinary KernelRecorded runs the logical count is the script-bar count; for
selected reports it is M+1, including the anchor (M=0 still has one point).
The logical count is not a completion witness.

`report.total_trades` and `metadata.trades.logical_count` are the same logical
T, including report-only open-at-end rows. `report.trades_len` is the resident
array length. Thus T equals the length only on the owning FULL route;
SUMMARY_ONLY and STREAMED return `total_trades=T`, `trades=nullptr`,
`trades_len=0`. Existing representability rules of ABI-4 scalar fields remain;
this revision adds no guessed row, bar or distinct-key limit.

## Every ReportC / pf_report_t field

“Same” below means the identical value/bits from the same FULL execution,
including NaNs, signed zero, undefined-statistic rules, selected presentation
and terminal rows. Public and internal spellings differ for the security
evaluation totals; both are listed. On failure the existing partial-report
rules apply, with `complete=0`; no complete summary is inferred from a prefix.

The FULL column describes retained sequences. The equity and optional-sequence
controls above can omit their respective resident histories in FULL: the
affected pointers/lengths are NULL/0 and `captured=0, external=0`, with logical
counts unchanged.

| Payload field(s) | FULL | SUMMARY_ONLY | STREAMED |
| --- | --- | --- | --- |
| `total_trades` | Logical T, equals resident row length | Same logical T | Same logical T |
| `trades`, `trades_len` | Owning ordered array, T | NULL, 0 | NULL, 0; all T rows external |
| `net_profit` | Existing report PnL | Same | Same |
| `input_bars_processed`, `script_bars_processed` | Existing counts | Same; count authoritative pass once | Same |
| `security_feeds_total` | Existing total | Same | Same |
| `security_eval_complete_total` / `security_complete_total` | Existing total | Same | Same |
| `security_eval_partial_total` / `security_partial_total` | Existing total | Same | Same |
| `magnifier_sub_bars_total`, `magnifier_sample_ticks_total` | Existing totals | Same | Same |
| `input_tf_seconds`, `script_tf_seconds`, `script_tf_ratio` | Existing timeframe metadata | Same | Same |
| `needs_aggregation`, `bar_magnifier_enabled` | Existing booleans | Same | Same |
| `security_diag`, `security_diag_len` | Owning per-site array | Same bounded per-definition array and ownership | Same |
| `trace`, `trace_len` | Owning enabled trace entries | NULL, 0 | NULL, 0; enabled entries external |
| `trace_names`, `trace_names_len` | Owning pointer array; borrowed strategy-owned strings | NULL, 0 | NULL, 0; enabled name association external |
| `metrics.all`, `metrics.longs`, `metrics.shorts`, `metrics.equity` | Existing complete 768-byte `pf_metrics_t` | Bit-identical complete block | Bit-identical complete block |
| `equity_curve`, `equity_curve_len` | Owning presented points, resident length; NULL, 0 under the equity-summary control, with logical count unchanged and capture/external flags 0 | NULL, 0; logical count in metadata, capture flag 0 | NULL, 0; logical count in metadata, capture/external flags 1 |
| `broker_state_hash`, `broker_state_hash_len` | Owning enabled hash sequence | NULL, 0 | NULL, 0; enabled hashes external |

Outputs remain outside ReportC. In FULL the existing output accessors retain
their behavior unless the optional-sequence control omits those histories.
SUMMARY_ONLY retains bounded manifest/constants/hlines and
summary values but no series, row times, alert messages or events as report
output; logical output counts remain in metadata. STREAMED sends those
enabled sequences through O/V/K records without rebuilding columns in RAM.
Per-run constants remain accessible as bounded metadata. Replay must never
duplicate outward alert delivery.

JSON summary scalars (`total_trades`, wins/losses, win rate, net PnL, average,
best/worst trade, trade-PnL drawdown and bars processed) preserve their current
values and zero-trade rules. Fold PnL drawdown with the existing
`cum += pnl; peak=max(peak,cum); max_dd=min(max_dd,cum-peak)` sequence;
do not substitute equity drawdown. Keep `sharpe_tv` and `sortino_tv` keys.
JSON may use empty sequence arrays only alongside the explicit envelope
metadata above. STREAMED serialization may read bounded pages to emit the
same full arrays; it must not reconstruct an owning full document first.

## Exact second replay for SUMMARY_ONLY

1. Before bar zero, bind the immutable input/spec/parameters, foreign feeds
   and columns, FX, chart calendar/timezone, random/time seeds and initial
   script state to an exact replay provider. A host with uncontrolled
   callbacks, nondeterminism or an arbitrary whole-array presenter is not
   eligible until it supplies an exact bounded-state adapter. Return
   `PF_REPORT_MODE_REPLAY_UNAVAILABLE` before execution where eligibility can
   be determined then; never silently remove metrics or switch to FULL.
2. Pass A is authoritative. Keep a finalizable held point and pre-tail fold
   snapshot so range-end/selected presentation enters folds only when final.
   Accumulate existing all/long/short trade folds, extrema, exposure,
   first/last coordinates and ordered monthly/bar return sums/counts.
   Preserve each operation and grouping. JSON summary folds run here too.
3. Finish the means and final bar density. Monthly rf is `.02/12`, bar rf is
   `.02/bars_per_year`; retain the existing annualization and NaN guards.
   With enough returns, pass B re-executes once from the same initial state
   and visits the same final presented points in order. Both monthly and bar
   folds consume that one ordered replay. Accumulate
   `var += (r-mean)*(r-mean)` and
   `d=min(0,r-rf); down += d*d` with the current denominators/guards.
   Welford, moments, compensated sums and regrouped partial sums are forbidden.
   If neither ratio needs pass B under the existing guards, state that no
   second-pass fold was required; do not fabricate a replay witness.
4. Gate publication on matching final logical trade count; the exact ordered
   trade witness using the existing canonical row bytes and all associated
   report identity/close metadata; logical point count; first and last
   coordinates (`time_ms`, equity bits, open-profit bits); and existing final
   continuation, broker and source/script hash witnesses applicable to the
   run. Preserve their domains, seeds, byte encoding and order. Include the
   selected anchor and presented terminal value. A commutative PnL checksum,
   count alone or approximate float comparison is insufficient. These are
   fixed accumulators, not stored witness arrays.
5. Pass B suppresses outward observers, alerts, output/sink publication and
   user notification side effects while preserving the internal semantic
   transitions/hash inputs the original pass performed. It never doubles
   processed-bar counters, trades or constants in the returned summary.
   An error or witness mismatch fails result assembly with
   `report_result_invalid`, `field="report.replay"`, `reason="witness_mismatch"`
   for a mismatch; it must not publish pass A as complete or reuse old data.
6. Freeze the validated summary. Repeated envelope reads copy it without a
   replay or callbacks. Do not hold the non-recursive metric timezone lock
   across engine dispatch: exact chart-month calculation needs a pure
   provider or a verified post-dispatch scope released before execution
   resumes. Preserve floating environment as well as calendar behavior.

Retain only immutable shared inputs and fixed per-run folds/replay state.
Do not restart execution for each indexed point. This is a fixed number of
ordered passes, not an O(1)-time claim. STREAMED may instead reread its external
curve through the existing bounded point source for the unchanged metric
folds; a per-trial scratch curve is never the SUMMARY_ONLY implementation.

## Value, page and visitor readers {#o1_report_readers_v1}

Include `<pineforge/report_pages.hpp>` for `LogicalIndexV1` (`uint64_t`),
`TradeRowV1`, `TradeTextV1`, `VisitResultV1`, `TradeVisitorV1`,
`EquityVisitorV1`, `TradeSourceV1`, `EquityPointSourceV1` and
`HeldEquityPointV1`. The four engine methods below are planned entry points
for `<pineforge/engine.hpp>`; they are not declared on `BacktestEngine`.
Their signatures, lifetime and mode rules below remain the planned contract:

| Native method | Logical population and availability |
| --- | --- |
| `completed_trade_source_v1()` | **planned: provider owed by the ledger lane.** Exact semantic completed rows, excluding report-only range-end rows. Available in every mode at a committed script callback boundary and after execution. Its `size()` counts the whole history, including retired rows. |
| `report_trade_source_v1()` | **planned: provider owed by the ledger lane.** Frozen final report rows: completed rows followed by range-end rows, in exactly the existing report order. Available when trades were captured in FULL/STREAMED. Its `size()` equals `metadata.trades.logical_count`, not `report.trades_len`. SUMMARY_ONLY fails explicitly. |
| `report_equity_source_v1()` | **planned: provider owed by the metrics lane.** Frozen final presented points, including the selected anchor. Available when equity was captured in FULL/STREAMED. Its `size()` equals `metadata.equity_curve.logical_count`, not the resident array length. SUMMARY_ONLY and an uncaptured curve fail explicitly. |
| `held_report_equity_point_v1()` | **planned: provider owed by the metrics lane.** One detached final presented point in every mode, including SUMMARY_ONLY; explicitly absent for zero logical points. It is not a captured curve. |

Source accessors return `std::unique_ptr<const ...>` **by value**. The caller
owns only a fixed-size reader adapter, never a row/page/sequence allocation.
Its engine context is borrowed until the next execution/configuration mutation
and only while the engine lives; read-cache refills keep the same logical
generation. Do not call a reader after either boundary; destroying an
adapter does not free a report payload or its engine. The engine must not
retain an ever-growing list of readers. Reader backing windows and indexes
have a population-independent bound fixed before bar zero. FULL may read its
existing owning storage; STREAMED reads the exact external captured source,
without constructing a second output tape or a resident sequence. Explicit
STREAMED support includes that bounded readback provider; the legacy sink's
write callback alone does not establish support for these declarations.

Successful source acquisition always returns a non-null adapter.
Report source/tail acquisition before validated completion throws
`std::logic_error`. A requested uncaptured report sequence also throws
`std::logic_error`, never an empty provider, implicit FULL allocation or
execution replay. A captured empty sequence is valid and has a zero-size
provider. Completed-history reads outside a committed boundary fail
explicitly; they must not expose a partially amended row. Callbacks must not
mutate, run or recursively dispatch the engine. These lifetime rules do not
permit returning a reference or pointer into backing pages. Detached rows,
points and caller-buffer bytes remain valid after refills, retirement and
adapter destruction.

`TradeRowV1.value` is the exact `pf_trade_t` value: timestamps, prices, PnL,
percentages, side, excursions, quantity, commission, bar indices and
`open_at_end`. Preserve all defined field bits, including NaN payloads and
signed zero; object padding is not a new wire format. The additional
`entry_incarnation` is exact; `close_cause` uses the existing resolved
`pf_close_cause_t` classification, while `exit_from_bracket` separately
preserves the raw 0/1 source label. Completed rows have `open_at_end=0`;
the report provider also covers its terminal suffix. Do not synthesize close
causes from IDs or reinterpret a margin row as a bracket row. The
`entry_incarnation` identifies an accepted request record; a definition
superseded before submission has none.

Both sources return `at(index)` **by value**. For `read_page(from,out,capacity)`,
the caller supplies the bounded buffer; the method copies ascending logical
indices and returns `min(capacity,size()-from)`. No pointer into the source is
returned. `from==size()` and `capacity==0` are valid. `out==nullptr` is valid
only with zero capacity, even at the end. Capacity is a transfer size, never
an admission cap or a reason to truncate the run.

`visit(from,count,context,visitor)` selects the logical interval
`[from,from+count)` and makes one forward ordered pass. Trades additionally
provide `visit_backward(from,count,context,visitor)` over **that same
interval**, starting at `from+count-1` and finishing at `from`. The full
newest-first position-tail scan is therefore:

```cpp
auto rows = engine.completed_trade_source_v1(); // planned: provider owed by the ledger lane
auto result = rows->visit_backward(0, rows->size(), context, visitor);
```

Backward order reverses logical indices, never a fresh sort by exit time.
Preserve ties, nonmonotonic times, side/origin/ID/margin filtering, the first
too-old exit-time barrier and the exact newest-first additions. The visitor
can stop at that barrier by returning false; it must not skip the barrier
and continue to earlier indices. No forward subtotal replaces that scan.
Neither provider traversal may accumulate a row list or restart execution
per row. Caller-owned copies remain the caller's explicit output storage.

`TradeVisitorV1` takes `(void*, LogicalIndexV1, TradeRowV1)` and
`EquityVisitorV1` takes `(void*, LogicalIndexV1, pf_equity_point_t)`, with
**by-value callback arguments**. A callback may retain its own copy; it cannot
edit the source. A false return stops after the delivered item:
`VisitResultV1.visited` includes it and `complete=false`, even when that was
the last item. A fully consumed range, including an empty one, has
`complete=true`. This flag describes only that requested traversal, not
envelope/run completion. Context may be null; the visitor may not.

Validate `from<=size()` and `count<=size()-from` before any copy/callback,
without evaluating a potentially overflowing sum first. Bad indices, ranges
and text offsets throw `std::out_of_range`; bad buffers, unknown text
selectors and null callbacks throw `std::invalid_argument`, including for an
empty traversal. Backing and callback errors propagate as failures, never
EOF, zero successful rows or a complete prefix. No logical index is narrowed
through the legacy report's `int` lengths; existing envelope scalar
representability rules remain unchanged.

Trade strings use `text_size(index,field)` and
`read_text(index,field,byte_offset,out,capacity)`. `TradeTextV1` selects
`EntryId`, `EntryComment`, `ExitId` or `ExitComment`. Transfer the exact stored
bytes, including embedded NUL, without normalization or an appended NUL.
Offsets/capacities are bytes, so a chunk may split UTF-8. The page end/buffer
rules also apply to text; `byte_offset>text_size()` is out of range. One long
ID/comment must not force a whole-string allocation in a row/page reader.

### Equity points, held tail and metric input

Each `EquityPointSourceV1` value is the exact final presented
`pf_equity_point_t` (`time_ms`, equity bits, open-profit bits). Repeated
forward visits preserve point order and the selected anchor; point count
and observation count remain distinct. `<pineforge/metrics_stream.hpp>`
declares the ordinary and explicit-observation-count `compute_equity_stats`
overloads for this source. Their implementation walks captured FULL/STREAMED
points in the existing five ordered folds through fixed buffers. It must
neither build an array adapter nor change arithmetic, calendar behavior or
terminal presentation.

SUMMARY_ONLY does not acquire an `EquityPointSourceV1` or create a private
curve tape. Internally the metrics owner sends finalized points to the same
`EquityVisitorV1` family during the fixed ordered execution/replay passes
described above. Keep the editable last point and its pre-tail fold snapshot
until successor/terminal/selected presentation makes the value final; do not
publish or fold a still-amendable value twice. All replay witnesses and the
timezone-lock rule still apply. Public report/held-point reads consume frozen
values and never trigger execution.

After validated completion, `HeldEquityPointV1.present=true` means `value` is
that one final presented point and `index=logical_point_count-1`. With zero
logical points, `present=false`, `index=0` and `value` is zero-initialized.
The flag does not certify completion or sequence capture:
SUMMARY_ONLY still has `equity_curve.captured=0`, `external=0` and a NULL/0
resident curve. A selected anchor-only result has one point at index zero,
not zero observations disguised as an absent point. Returning the held point
does not authorize a historical point lookup in summary mode.

## JSON envelope and fingerprint

Explicit mode responses add `report_contract: "pineforge-report/v1"` and
`report_capture`, the JSON rendering of metadata: `version`, mode name,
`complete`, and the seven sequence objects with `logical_count`, boolean
`captured` and boolean `external`. C-only `struct_size` is not serialized.
Legacy invocations add neither key. These versions are independent of
selected-window wire version 1, phase version 1 and outputs schema v1.

For an explicit mode, write the same fixed policy object to `applied_runtime`
and fingerprinted `provenance.runtime`:

```json
{"report_contract":"pineforge-report/v1","report_mode":"SUMMARY_ONLY","sequence_capture":false}
```

`sequence_capture` is true for FULL/STREAMED and false for SUMMARY_ONLY.
Set both `fingerprint.version` and `fingerprint.provenance.schema_version` to
**3** for these explicit-mode reports, ordinary and selected alike. Retain the
existing canonical JSON/hash algorithm. The added policy/version bytes change
the fingerprint/token/digest; equality with the legacy fingerprint is not
promised. Legacy unrequested reports retain their existing versions and
bytes. Input/execution identity and broker/source hash leaves stay unchanged;
the output-envelope change is not permission to change them. New durable
execution state introduced by implementation must be hashed or have a named,
reviewed output-only waiver.

### Selected-window/v1 with a named report sink {#o1_selected_window_named_sink_v1}

`docker/run_json.py` accepts
`--report-policy selected-window/v1 --report-sink PATH --run-id ID --spill-dir DIR`
alongside the required strategy, chart and selected-window arguments. With a
sink-v1 library this combination is supported: naming the sink is neither
refused nor silently ignored. The caller receives the selected report
projected to the summary JSON shape. This is the existing sink transport,
independent of the native SUMMARY_ONLY mode described above.

The projection replaces the `trades` key with a `report_sink` reference at the
same position and drops `equity_curve`. Both arrays are **absent**, not empty
arrays or null values. This is a projection rule, not truncation: the complete
selected trades and presented equity points remain in the sink file.
`report_window` and `report_shape` remain present and unchanged; in particular,
`report_shape` retains the `full/v1` description and its logical trade/point
counts even though the JSON arrays are omitted.

The reference has exactly these keys, in the writer's order:

| Key | Type and value |
| --- | --- |
| `path` | String: the caller's sink path. |
| `schema` | String: `"pf-report-sink"` (the file contract is `pf-report-sink/v1`). |
| `version` | Integer: `1`. |
| `run_id` | String: the request's `ID`, copied unchanged from the sink header; default `""`. |
| `counts` | Object: nonnegative integer counts keyed by each enabled record tag, including zero counts, in header descriptor order. |
| `sha256` | String: 64 lowercase hexadecimal digits for the raw header and data-record lines, including each LF and excluding the trailer. |

The remaining top-level fields are `engine`, `input`, `applied_inputs`,
`applied_overrides`, `applied_runtime`, `elapsed_seconds`, `summary`,
`diagnostics`, `metrics`, `report_window`, optional `outputs`, and
`fingerprint`. Their selected-report values are preserved, with the existing
wall-clock values in `elapsed_seconds` and `diagnostics.phase_timing`.
`applied_runtime.report_window` and
`fingerprint.provenance.runtime.report_window` carry the same window object
as top-level `report_window`. The selected fingerprint's `version` and its
provenance's `schema_version` remain 2.

`summary` contains `total_trades`, `wins`, `losses`, `win_rate_pct`, `net_pnl`,
`avg_trade`, `best_trade`, `worst_trade`, `max_drawdown` and `bars_processed`,
with the existing selected-report folds and zero-trade rules. `metrics`
retains the complete `all`, `longs`, `shorts` and `equity` objects, including
the JSON names `sharpe_tv` and `sortino_tv`. Dropping the JSON curve does not
drop equity-derived metrics.

With `--outputs`, `outputs` contains exactly these six O(1) members in order:
`schema_version`, `message_format`, `manifest_sha256`, `manifest`, `constants`,
`hlines`. Their sizes are bounded by the script definition, independent of
observed bars/trades/events. `outputs.bars`, `outputs.series` and
`outputs.events` are absent; their histories remain external. Without
`--outputs`, the entire `outputs` key is absent.

The sink file is UTF-8 NDJSON: one header object, tagged array records and a
final trailer object. The header's keys, in order, are `schema`
(`"pf-report-sink"`), `version` (`1`), `run_id` (the caller's string), `engine`
(the engine version string) and `records` (an array of descriptors). Each
descriptor has `type`, `fields`, `types`, `units`, in that order; each data
array starts with its tag and then follows that descriptor's column order.
The enabled descriptors occur in the following order; this does not require
data records to be grouped by tag:

| Tag | Records carried for this request |
| --- | --- |
| `E` | All selected presented equity points (`time_ms`, `equity`, `open_profit`), including the anchor: M+1 points for M window script bars. |
| `T` | All selected report trades in report order, including report-only `open_at_end` rows. |
| `O` | Recorded output row times and series-slot values when `--outputs` is enabled. |
| `V` | Recorded output events when `--outputs` is enabled. |
| `K` | One run-constants record when `--outputs` is enabled, including for an empty output run. |
| `X` | Trace entries when trace recording is enabled. |
| `H` | Broker-state hashes when hash recording is enabled. |

`E` and `T` are always declared; optional tags are declared only when enabled.
The trailer has `trailer` (`"pf-report-sink"`), `version` (`1`), `counts` and
`sha256`, in that order. The reference uses the verified trailer counts and
digest. Before preparing the JSON, the harness checks the entire file,
`run_id`, and the engine's complete sink receipt, including trade count.

The engine creates or truncates `PATH` when it writes the header
(`O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC`); the harness passes the named path
without first creating a temporary sink. The named file persists for the
caller after report/state cleanup. `--run-id` is an opaque caller identifier,
not a generated ID. `--spill-dir DIR` supplies the engine's `scratch_dir` and
the directory for one unlinked, prepared JSON document. The harness serializes
and flushes that document before publishing any report bytes, and publishes
after native cleanup succeeds. It creates no per-output column temporaries
for this projection. `DIR` must be an existing writable directory; when
omitted, it defaults to the system temporary directory. The named sink stays
at `PATH`, independently of `DIR`.

For any of `--report-sink`, `--run-id` or `--spill-dir`, embedded NUL or text
that cannot encode as UTF-8 fails before library loading with
`run_request_invalid` and `args == {"option": "arguments"}`. A library missing
the sink API refuses an explicit named-sink request before execution with
`strategy_library_incompatible` and `args == {"reason": "symbol_missing"}`;
it does not fall back to owning arrays. A partial sink API or a sink version
other than 1 fails with `engine_invariant`.

The combination is pinned by
`tests/test_run_json_selected_sink.py::test_selected_named_sink_yields_the_summary_projection`.
That test checks both output settings, the complete projection, reference key
order, sink counts/run ID, persistent named path and prepared-document
directory. `REFERENCE_KEYS` and `OUTPUT_KEYS` in
`tests/test_run_json_sink_summary.py` pin the two ordered member lists.
See the [JSON field types and presence table](../pages/report-schema.md#report_selected_window_named_sink_v1).

## Integration and ABI inventory

Native C++ mode selection and envelope reads implement the runtime admission,
capture metadata and completion checks described above. The envelope adds
**zero** new `PF_API` declarations/definitions.
`include/pineforge/pineforge.h` has 102 public declarations.
`scripts/check_c_abi_runtime.py` pins 113 declarations across its four
`RUNTIME_HEADERS` and 93 runtime implementations; `scripts/test_ci_verify.py`
pins the same 113/93.
The separate native-C inventory remains 43 declarations / 43 implementations.
There is no ABI-4 struct/count bump, README symbol-table change or update to
legacy ctypes report strides.

If the HPO/transport host requires a runtime C bridge, its implementation
owner must update the runtime file-header inventory, public declarations,
`EXPECTED_RUNTIME`, both hardcoded counts, the assertions in
`scripts/test_ci_verify.py`, all ctypes bindings and the public ABI/report
documentation together. Do not add a declaration-only PF_API probe or treat
the C-compatible envelope as an already exported function.

Batch-B already supplies sink-v1 logical trade totals/NULL arrays,
scratch-backed ordinary/selected curve access, ordered streaming pieces,
sealed trade folds and position-tail spill. These address parts of inventory
R01/R03/R04/R06/R07/R09/R11/R13/R14/R15/R17/R20/R21/R22; they do not establish
summary replay or a general resident bound. R05 and R10's exact fixed-state
folds are reused. This B3 slice changes the contract for R02/R04/R13/R14/R15/
R16/R17/R18/R20/R21/R22 and documents the R01/R03 replay dependency. It does
not claim to remove any runtime allocation.

Round 2 adds the R07/R11/R13 row-reader and R01/R03/R04 point-reader contracts
on top of that B3 work. On the round-1 tree these declarations are absent;
the legacy row APIs still return references into resident storage. The new
contract requires detached values, bounded transfer buffers and full logical
history with both scan directions. It changes no legacy method or backing
container in this lane. The runtime resident bound and old-reader migration
remain implementation-lane acceptance work; no new durable execution state
or hash waiver is introduced here.

Acceptance must include a real populated FULL fixture, refusal of SUMMARY_ONLY
on an unsupported provider with the diagnostic above, successful SUMMARY_ONLY
with all metrics/scalars compared to FULL, counts/capture checks, STREAMED
receipt/order equivalence, replay-side-effect/mismatch tests and charged-memory
growth tests in the implementation lanes. An unsupported provider cannot
count as successful SUMMARY_ONLY coverage.

Round 2 extends `test_o1_report_contract` with TEST-ONLY vector providers
implementing the abstract reader interfaces. Four explicit completed rows
exercise by-index values, bounded pages, forward/backward passes, ties and
nonmonotonic exit times, the tail barrier, exact double bits, callback
cancellation and invalid/overflowing ranges. Four points exercise ordered
visits, bounded copies and detached value lifetimes. These are declaration
and consumer-behavior checks, not a storage/memory or runtime-support claim.
Against the round-1 header closure the unit must fail to compile because
`report_pages.hpp`/its visitors do not exist. Record the actual first compiler
diagnostic remotely as described in the fixture README. No RED or GREEN
receipt has been executed by this lane.

## Appendix: a1ctr names -> contract names {#a1ctr_names_to_contract_names}

No round-1 name is renamed. The stopped a1ctr draft is an older-base reference,
not an alternate public contract or a compatibility alias header.

| a1ctr draft name | Contract name / disposition |
| --- | --- |
| `<pineforge/report_output.hpp>` | `<pineforge/report_mode.h>` for modes/envelope; `<pineforge/report_pages.hpp>` for C++ value/page/visitor readers; `<pineforge/engine.hpp>` for native entry points. No `report_output.hpp` is installed. |
| `PINEFORGE_HAS_REPORT_OUTPUT_V1` | Not adopted. Header/macro presence must not advertise runtime support; negotiate the implemented `pineforge-report/v1` provider. |
| `OutputModeV1::LegacyOwningArrays` | `ReportMode::FULL` / `PF_REPORT_MODE_FULL` (`0`). |
| `OutputModeV1::Summary` | `ReportMode::SUMMARY_ONLY` / `PF_REPORT_MODE_SUMMARY_ONLY` (`1`). |
| `OutputModeV1::Paged` | `ReportMode::STREAMED` / `PF_REPORT_MODE_STREAMED` (`2`), requiring the bounded external sink/readback contract. Pages are a transfer interface, never a fourth mode. |
| `OutputOptionsV1`, `set_report_output_v1(options)` | `BacktestEngine::set_report_mode_v1(ReportMode)`. The mode word is the sole policy selector; the accepted mode appears in envelope metadata. |
| `OutputOptionsV1.version`, `SummaryV1.version` | `PF_REPORT_ENVELOPE_VERSION` (`1`) and initialized `metadata.struct_size` / `metadata.version`; not an additional options/summary version. |
| `HistoryDeliveryV1::{Omit,External}`, optional-history fields of `OutputOptionsV1` | The selected mode's capture policy plus existing recording switches and sink-v1 configuration. SUMMARY_ONLY always omits report histories; no separate per-history mode vocabulary is adopted. |
| `StorageKindV1`, `StorageActionV1`, `StorageEventV1`, `StorageObserverV1`, `storage_context`, `storage_observer` | Not adopted as report-policy API. Allocation instrumentation belongs to the implementation lanes' allocation and growth tests; no substitute public names or new durable state are introduced here. |
| `SummaryV1`, `report_summary_v1()`, `visit_report_summary_v1(...)`, `SummaryVisitorV1` | `ReportEnvelopeV1` / `pf_report_envelope_v1`, read with `fill_report_envelope_v1(...)`. Reuse its frozen payload/metadata; no second summary type or summary visitor. |
| `SummaryV1.mode`, `total_trades`, `equity_point_count`, optional sequence counts | `metadata.mode`, `metadata.trades.logical_count` (and `report.total_trades`), `metadata.equity_curve.logical_count`, and the corresponding five optional sequence `logical_count` fields. |
| `SummaryV1.completed_trades` | **planned: provider owed by the ledger lane.** `completed_trade_source_v1()->size()`, excluding the report-only suffix; no second count is added to the 128-byte metadata. |
| Other `SummaryV1` report counters, timeframe fields, `net_profit`, `metrics` | Existing `envelope.report` fields, with the exact round-1 field table above. |
| `SummaryV1.summary_avg_trade`, `summary_best_trade`, `summary_worst_trade`, `summary_max_drawdown` | Existing JSON `summary.avg_trade`, `summary.best_trade`, `summary.worst_trade`, `summary.max_drawdown`, respectively; no additional C POD. Drawdown stays the trade-PnL-prefix fold. |
| `LogicalIndexV1`, `TradeTextV1`, `TradeRowV1`, `VisitResultV1`, `TradeSourceV1`, `EquityPointSourceV1` | Same names under `pineforge::report`, declared in `report_pages.hpp`. Indexed rows/points remain values. |
| `TradeVisitorV1`, `EquityVisitorV1` | Same names, but callbacks take detached row/point **values**, replacing the old draft's const-reference arguments. |
| `completed_trade_source_v1()`, `report_trade_source_v1()`, `report_equity_source_v1()` | **planned: provider owed by the ledger lane for each trade source and by the metrics lane for the point source.** Same method names; return caller-owned `std::unique_ptr<const ...>` adapters by value, replacing borrowed reference returns. No resident page/row pointer is exposed. |
| `metrics::compute_equity_stats(const report::EquityPointSourceV1&, ...)` | Same overload names, declared in `metrics_stream.hpp`, using the sole point family and unchanged point/observation denominators. |

`TradeSourceV1::visit_backward(...)`, `HeldEquityPointV1` and the planned
`BacktestEngine::held_report_equity_point_v1()` (planned: provider owed by the
metrics lane) are round-2 additions with no a1ctr or round-1 name replaced.
