# O1 selected and observed capture handoff

Base: `583c2217d2524e8f2bc4c8a77dfa336309d741fc`.
Rows: R03 and R04. This slice is groundwork plus capture-time metric freezing;
it does **not** establish working SUMMARY_ONLY execution or close R03/R04.
No remote fail-before, build, unit, corpus or charged-memory result has been
obtained. No command requiring a local compiler or script was run.

## Runtime changes

At the base, `capture_presentation` copies selected M+1 or ordinary observed
Bs points into its independently owned report unless a sink supplies scratch
storage. `fill_selected_report` computes metrics again at each read, using
the captured points but current engine metric inputs. Its sink-off array
copy covers max(published count, metric count).

The capture now computes the complete existing `pf_metrics_t` after the one
presentation hook and before publishing the capture. The trade blocks use
the same closed-row prefix and final open-row order. Selected sink points
are validated before metric computation, with the same typed errors and
precedence as the existing final selected-sink scan. A report read copies the
frozen metric block, with no metric source, trade fold or timezone walk.
Removing a sink still explicitly requests the legacy full array; its
materialization remains available and uses the independent captured source.

`SelectedCaptureLatches` separates final-value validation from the writer.
Feed it final execution points in their recorded order. For a host that
rewrites arbitrary earlier points, scan its final array at capture; do not
latch provisional values. Its copyable scalar state preserves trade,
open-lot, account, contiguity, then count refusal order. The existing selected
sink validates its edited metric observations too, including a suffix hidden
from publication; the anchor is excluded from observation validation.

`PresentationCaptureWindow` owns two points, two observation/exposure flag
pairs, a logical count and a held count. `append_final` releases the oldest
point directly to the existing scratch writer before reusing its slot.
`append_metric_point` supplies the same release interface with the original
observation and exposure flags for the forthcoming exact metric passes.
`emit_metric_tail` visits every edited held point, including hidden points.
`published_count` subtracts only the number hidden by the hook. None of these
operations retains the released prefix, allocates a growing collection or
introduces a population cap.

The resident state of these helpers is fixed by their types, independent of
bars, observations, trades or keys. The frozen metric block reuses the
existing 768-byte field in the capture. Existing scratch writers/readers each
have a fixed 64 KiB window. FULL capture and FULL report arrays are still
O(M) / O(Bs); scratch-backed STREAMED output remains a per-point external
sequence. Neither qualifies as SUMMARY_ONLY.

Batch-B already supplied finalizable sink recording, L17a selected validation
latches, the two-point selected presentation, L9s sink assembly and bounded
scratch readers. Those pieces were extended rather than replaced. The
range-end held-point rewrite and extrema prefix restore were already present;
this lane did not change equity recording, sealing, digests or their headers.

## Exact metric integration boundary

The metrics lane's `metrics_fold.hpp` and `tests/test_metrics_fold.cpp` WIP
specify the calls below. The new capture test includes that header and
exercises both exact passes through the held window, comparing all 768 bytes
to the frozen array oracle for 96 additional curves. That unit requires the
supervisor to consume the metrics lane's header and implementation first;
neither has been copied into this lane.

The expected runtime integration calls, already exercised by the new unit
using that WIP's exact spellings, are:

1. Construct `metrics::EquityMetricsFold first` before execution.
2. Insert the selected anchor with
   `window.append_metric_point(anchor, false, false, emit_first)`.
   Insert each final selected observation with its actual exposure flag;
   ordinary observed capture has no synthetic anchor.
3. In `emit_first(point, observation, in_market)`, call
   `first.fold(point, metrics::equity_month_key(point.time_ms, chart_timezone),
   observation, in_market)`. The held terminal value is supplied only after
   execution rewrites, and the held presentation window only after its edit.
   Finish that window with `window.emit_metric_tail(emit_first)`.
4. Take `const auto pass_a = first.first_pass()`. Bind
   `metrics::EquityMetricInputs{initial_capital, first_open, last_close,
   net_profit}` with the selected first/last input prices and M denominator.
   The anchor's observation flag is false, including M=0 and M=1.
5. Construct `metrics::EquityMetricsSecondPass second(pass_a)` and walk one
   exact deterministic replay. Its visitor calls
   `second.fold(point, metrics::equity_month_key(point.time_ms, chart_timezone),
   observation, in_market)` over the same final presented points.
6. Obtain `const auto ratios = second.finish()` and require
   `ratios.matches(pass_a)`. This is additional metric evidence; it does not
   replace the contract's ordered trade and final execution/source witnesses.
7. Freeze `metrics::finalize_metrics(pass_a, inputs, all, longs, shorts,
   &ratios)` into the capture's existing metric block. A repeated report read
   performs none of these calls.

Do not infer exposure from nonzero open profit. Do not hold a non-recursive
timezone guard across dispatch or replay. The recording lane must preserve
the two still-editable points and their flags until presentation is final;
folding the penultimate selected point before its possible edit is incorrect.

Still needed from the runtime integration owner: the stored/admitted
SUMMARY_ONLY mode; its selected/observed capture branch and frozen logical
counts; the pre-bar-zero point/replay provider; execution and trade witness
gates; exact exposure flags; and output suppression. These require engine or
consumer declarations outside this lane's assigned function ranges. No new
per-run global registry, alternate mode API or implicit fallback was added.
The integration owner must also update the read-time metric descriptions in
`src/native_execution_consumer.hpp` and `include/pineforge/metrics_stream.hpp`;
this lane did not edit those shared headers.

## Presentation hooks and compatibility

The inventory below comes from `git grep` for `present_report(` across
include, src, tests and examples. C callback hook forwarding does not expose
an additional whole-array presentation hook.

| Hook | Reads or edits |
| --- | --- |
| `BacktestEngine::present_report` in `include/pineforge/engine.hpp` | Default no-op. |
| `PineStrategyHost::present_report` in `src/source/pine_strategy_host.cpp` | Final equity point; presented open-at-end trade rows in lot order; `total_trades`, `trades_len`, `net_profit` and closed-row PnL. It re-marks terminal rows, updates net PnL and rewrites the final point. It does not inspect equity history. |
| `tests/test_report_sink_equity.cpp` fixture | Counts the presented length, delegates to Pine, edits the final point or shrinks the curve. |
| `tests/test_report_sink_scratch_curve.cpp` fixture | Edits only the final point. |
| `tests/test_report_sink_selected_l17a.cpp` fixture | Counts calls, reads/edits the last point and cuts one point; can throw. |
| `tests/test_selected_metrics_l17b.cpp` `Host` | Reads presented trades and the window; edits its last two points. Its explicit legacy branch reads/edits the full curve's middle, last point and published length. It also snapshots engine history as a test oracle. |
| Same file `LiveReportHost` | Counts calls and reads the scratch descriptor; no equity-array read. |
| Same file `PineHost` | Counts calls and delegates to Pine's terminal presentation. |
| Same file `InvalidWindowHost` | Deliberately replaces storage, grows/negates the length or throws after replacement. |
| Same file `StorageHost` | Reads the presented first and last points and length. |
| Same file `TradeStorageHost` | Reads/edits open-at-end trade rows, net PnL and final equity point. |
| Same file `RandomCaptureHost` added here | Edits the last two points and hides zero, one or two points. |
| `tests/test_selected_window_report_integration.cpp` fixture | Counts calls, throws or sets the published length to one. |
| `tests/test_selected_production_routes.cpp` and `tests/test_selected_window_routes.cpp` fixtures | Record the presentation event; no array reads. |

FULL continues to invoke the full-array hook on the full-array route,
including arbitrary middle edits and shrink/hide behavior. Existing sink
hooks retain their bounded view. Summary mode must require an explicit
bounded-presentation declaration and an exact replay provider before calling
any custom hook. The conservative default for an arbitrary override is
whole-array access; C++ cannot discover which indexes an override will read.

For a whole-array-only presenter, summary negotiation must return
`PF_REPORT_MODE_REPLAY_UNAVAILABLE` before execution, as specified by the
contract lane's `docs/design/o1-report-contract.md`, “Exact second replay for
SUMMARY_ONLY.”
The provider/transport must surface the requested mode's documented error
rather than invoke the hook with a shorter array. This declaration/error
boundary and the required successful-summary-versus-FULL hook test remain
unimplemented here; a documentation entry is not runtime coverage.

## Verification and remaining acceptance

No tests in this slice have been executed. Expected fail-before behavior:

| Test | Expected failure on the base |
| --- | --- |
| `test_selected_metrics_l17b` / `captured_metrics_are_frozen` | Repeated reads recompute percentages/exposure/CAGR from deliberately changed execution metric inputs, changing the raw metric block. |
| Same executable / `frozen_sink_reads_need_no_metric_storage` | A selected read allocates a `SpillCurve` reader and trade folds; an observed read allocates a scratch reader and trade folds. The allocation-refusal interval detects both. |
| `test_selected_capture_latches` | The frozen previous-epoch header closure lacks the new latch/window and metric-fold headers. The test also compares all failure combinations and 2,048 random arrays against the independent original scan. No remote first diagnostic is recorded yet. |

The added random engine captures assert actual window-script counts, compare
all 768 metric bytes to frozen array reference functions across 72 curves
including M=0/M=1 and three timezones, and compare every published sink point.
The helper tests assert 100,000 and 400,000 consumed points before checking
allocation counts. These helper populations are **not** the requested real
script-bar SUMMARY_ONLY growth test. The inherited selected sink fixture
asserts real M at 1,000 and 1,000,000 bars; it is already bounded in Batch-B.

Outstanding: real SUMMARY_ONLY capture at M=100,000 and M=400,000 with a
growing-before/fixed-after receipt; whole-array presenter negotiation in
SUMMARY_ONLY and successful FULL behavior; complete B1 replay/witness
integration; and all remote verification. Do not promote R03 or R04 to a
complete O(1) verdict from this slice.

Lab CTest rows to run are `test_selected_capture_latches`,
`test_selected_metrics_l17b`, `test_report_sink_selected_l17a`,
`test_selected_window_report_integration`, `test_report_sink_equity`,
`test_report_sink_scratch_curve` and `test_selected_production_routes`.
The supervisor must also run preflight, complete all-target release/debug
verification and the full corpus on the final integrated tree.

No ABI exports, declaration counts, existing document anchors or hash leaves
were changed. The new latches/window are local report state, not new durable
execution state. The existing report-only metric field remains under its
existing output-state waiver. Other lanes must preserve the bodies of
`capture_presentation` and `fill_selected_report`, the two new internal
headers, this document, the new test unit/registration and the added capture
tests when merging. The only edits outside the owned function bodies are
their two include directives; no other lane's function or header was edited.
