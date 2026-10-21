R01/R02 equity implementation and R05 second-pass math are prepared for
supervisor integration. R06 and R10 were already implemented by batch B.
This is an uncommitted, unqualified lane diff, **not a completed SUMMARY_ONLY
provider**. Terminal refold, selected capture, omitted trade rows, the versioned
envelope and deterministic engine replay still need their owners' integration.
No build, test, preflight, corpus or campaign result is claimed.

HEAD/base: `583c2217d2524e8f2bc4c8a77dfa336309d741fc`, branch `o1w/metrics`.
No git add, commit or push. `git diff --stat <base>..HEAD` is empty because
the supervisor commits this working diff. At handoff the three tracked files
have 191 insertions / 6 deletions; the new header, two test units, lane CMake
file and this report are additional untracked files. Use the working-tree
diff, including those new files, for review.

**Rows and bounds**

| Row | Base behavior | Prepared change / resident bound | Evidence still required |
| --- | --- | --- | --- |
| R01 | `engine.hpp:587,1381`; `native_execution_consumer.cpp:13166` grows the ordinary sink-off curve. Batch B already holds only one point when scratch-backed sinking is enabled. | `record_equity_point` selects the summary branch by the active equity mode. One vector slot is reserved before the first callback; previous FULL capacity is released at run preparation. A fixed first-pass prefix excludes the editable held point. Snapshot/presentation replaces that point before folding it; no subtraction or regrouping. Fixed broker-extrema prefix is separately seeded from initial capital, while metric extrema seed from the FIRST equity point. | New allocation and math tests; terminal helper hook below; lab qualification. |
| R02 | Ordinary `engine_report.cpp:272` allocates/copies `n` points. Batch-B ordinary sinking instead presents one point and rereads scratch. | Summary `fill_metrics_section` presents one stack point, finalizes fixed folds and returns `nullptr/0` resident curve. `equity_logical_point_count()` and `equity_summary_snapshot()` expose the separate logical count. There is no n-sized curve allocation, temporary curve, scratch writer or curve region in summary preparation. | Envelope must publish logical count with captured=0/external=0 and gate completion. R13 still owns removing trade arrays. |
| R05 | `engine_metrics.cpp:54-125,383-414` already generates month/bar returns twice with fixed auxiliary state. | New pure `metrics_fold.hpp` provides the exact ordered first sums/counts and second variance/downside walks. Both series share one point replay and use final bar density. Finalizers return the existing 768-byte `pf_metrics_t`. Storage is `sizeof(EquityMetricsFold)` plus `sizeof(EquityMetricsSecondPass)` and fixed snapshots/results, independent of point/return/month counts. Sizes are printed by the new math test. | Raw-byte differential test, floating-environment review, actual engine replay by its owner. |
| R10 | `TradeStatsAccumulator::State` and the percentage prefixes are already fixed. | No production change needed. `closed_trade_report_` is allocated at consumer line 3015, before `on_native_run_begin` at 3073; its ALL/LONG/SHORT pimpl allocations are fixed. `finish_streamed_trade_metrics:13549` copies three fixed states, then folds resident/terminal rows in order. Percentage prefixes remain scalar. | Existing independent accumulator test plus new allocation census: fold 1,000/100,000 rows with zero new allocations, compare fixed copy/finish allocation bytes and calls. |
| R06 | Contrary to the older inventory, batch B includes `src/metrics_spill_curve.cpp`. | Already implemented: one array of 2,730 binary points (65,520 bytes), bounded positional refills, constructors/readers and both explicit `SpillCurve` metric instantiations. Untouched. Summary conflicts with a sequence sink and returns from preparation before creating a writer/curve region. | Run existing spill/scratch tests remotely. No PASS inferred from source presence. |

**Pure interface for R03/R04 and replay**

All types below are in `pineforge::metrics`; the new header includes no engine
header. It accepts an externally supplied civil-month key, so a caller need
not hold the process timezone mutex across engine dispatch.

- `equity_month_key(time_ms, chart_timezone)`: existing UTC month arithmetic,
  or a local-month lookup within its own short timezone guard. Same truncated
  whole-second convention as the real array path.
- `EquityMetricsFold::fold(point, month_key, observation, exposed)`:
  closes the preceding held point and holds the next. Defaults are an ordinary
  observation and no exposure. For a selected anchor pass false/false.
- `replace_last(point, month_key)`: rewrites only the held point, including its
  time/month/open P&L. Returns false when empty.
- `first_pass()`: repeatable value snapshot; closes the held final month only
  in the returned copy. Includes logical/observation/exposure counts,
  first/last coordinates, metric extrema, return counts/sums and an ordered
  report-point witness. M=0 is represented by one non-observation anchor.
- `EquityMetricsSecondPass(first_snapshot)`: fixes means, risk-free rates and
  density from pass A. Feed the same `fold`/`replace_last` interface in pass B.
  `finish()` is repeatable and returns `EquityRatioResult`.
- `EquityRatioResult::matches(first_snapshot)`: checks this equity walk's
  count, endpoints, ordered point/month/classification witness and exact
  return sums/counts. Its `complete` flag is ONLY about this equity walk.
  It cannot replace the host's required trade/continuation/source witnesses.
- `finalize_equity_metrics` / `finalize_metrics`: same guards, divisions,
  pow/sqrt expressions and order within each return sum as the array path.
  With no matching second result they produce explicitly first-pass-only
  fields; `needs_second_pass()` and the engine's readiness flag prevent an
  envelope owner from certifying such a result as complete.

Engine integration is intentionally an equity component, not a provider probe:
`set_equity_report_mode`, `equity_report_mode`,
`equity_logical_point_count`, `equity_summary_snapshot`,
`equity_summary_ratios_ready`, `install_equity_ratio_result`, and the protected
`refold_summary_equity_tail` hook. The mode choice takes effect on the next
admitted run, not on an older report. Unknown modes and busy/conflicting
configuration are refused without changing the pending equity mode.

**Required supervisor / sibling integration**

1. Merge the contract lane's enum/header/envelope declarations. This lane's
   `ReportMode` uses the exact FULL=0 / SUMMARY_ONLY=1 / STREAMED=2 words,
   values and uint32_t underlying type from its new document. Keep one enum;
   prefer the contract declaration using its public C constants. The two
   additions at the end of `ReportC` overlap mechanically.
2. The versioned setter must establish exact replay/presentation eligibility
   and every sequence's omission policy before delegating to
   `set_equity_report_mode`. Do not advertise a summary-capable provider from
   this equity helper or its internal compilation macro. The envelope owner
   must bind `metadata.equity_curve.logical_count`, capture flags, current
   attempt ownership, and completion. Legacy report layouts are unchanged.
3. At the START of `BacktestEngine::refold_equity_tail` in
   `src/native_execution_consumer.cpp:13227`, add the equivalent of
   `if (refold_summary_equity_tail()) return;`.
   Its current sink-off full-array walk sees only the held point in summary
   mode and would lose the earlier broker extrema. This is a required hook,
   not an optional optimization. I did not edit that out-of-scope function.
4. R03/R04 must feed their own selected first pass, including the anchor and
   final presented observations, while preserving the existing validation
   order. The current selected/observed capture/read functions still use their
   arrays and bypass ordinary `fill_metrics_section`; they are untouched.
   Connect the pure fold at their authoritative point cadence before dropping
   those arrays. Freeze the validated summary and copy it on envelope reads,
   without another callback, presentation or replay.
5. R13 must remove report trade arrays and populate the fixed trade blocks
   before equity finalization. For the current ordinary array producer, this
   lane reads its presented trade array once through existing accumulators.
   If an omitted-row producer supplies `total_trades>0`, it must already
   supply the ALL/LONG/SHORT blocks; the ALL count is checked. This lane does
   not claim O(1) report storage in T before that producer lands.
6. Replay/HPO supplies finalized points to the second accumulator, validates
   all required trade/continuation/source witnesses and suppresses outward
   side effects. It can install a matching `EquityRatioResult` into ordinary
   equity assembly, or call the pure finalizer for selected capture. This
   lane implements no engine replay and does not fabricate its witnesses.

These integrations are outside the named function ranges. No native-consumer,
source-layer, frozen-header, C ABI, transport or corpus file was edited.
The additive equity setup in `begin_report_sink` is necessary to release old
curve capacity and reserve the held point before callbacks, including sink OFF.

**Tests and fail-before status**

`test_metrics_fold` compares all 768 bytes to the REAL production array
entry points, and pins those array results to the existing frozen
`equity_stats_reference.hpp` / `trade_stats_reference.hpp`. First-pass-only
comparison masks only the array oracle's four ratio fields. Full comparison
uses the second accumulator. Fixtures include empty/one/two points, M=0
anchor, zero/negative previous values, NaNs/infinity/signed zero, constant
returns, zero/reversed spans, large alternating equity, month boundaries,
UTC/Etc-UTC/New York/Taipei, deterministic random curves, terminal and
presentation replacements crossing months, intermediate/repeated reads,
partial second walks and reordered-point rejection.

Its intended first diagnostic against the frozen `583c2217d2` header closure
is missing `pineforge/metrics_fold.hpp`. **Not executed; no recorded compiler
diagnostic or fail-before receipt exists yet.**

`test_summary_metrics_allocations` replaces new/new[] (including aligned
forms), measures report allocation calls and requested bytes, and asserts an
identical result for 1,000 and 100,000 bars. It first verifies completed
execution, callback count, every-bar ordered close/time witness, last
timestamp, report bar count and zero-trade population. It checks held capacity,
null resident curve, no scratch descriptor, logical count, ratio readiness and
768-byte equality after feeding the FULL oracle's final points into the
second accumulator. That feed is test-only; it is not engine replay. It also
covers one/two points, presentation and FULL-to-summary handle reuse.

This allocation unit compiles against the frozen base without the new
internal feature macro: its requested summary fixture then exercises the
actual FULL path. Both populations still run; the null-curve/capacity and
equal-report-byte assertions must fail, while its FULL control demonstrates
the additional 99,000 * 24 curve bytes. This is a real growth RED, not a
disabled/xfail row. **Not executed; actual RED and GREEN receipts are pending.**
Its additional R10 census verifies zero allocation during trade folds and
fixed copy/finish costs at both trade populations.

CTest names to run remotely:
`test_metrics_fold`, `test_summary_metrics_allocations`, `test_metrics`,
`test_metrics_l4d`, `test_metrics_point_source`, `test_trade_stats_accumulator`,
`test_metrics_spill_curve`, `test_report_scratch`,
`test_report_sink_scratch_curve`, and the selected-report suites after sibling
integration. Required full preflight/release/debug/unit/ABI/corpus/parity work
belongs to the supervisor's lab run on the integrated exact tree. No campaign
sweep was run from this lane.

Local checks: git whitespace checks on tracked and new source/test/CMake
files were clean. Only file reads, git reads and file edits were performed.
There is no exposed lab/SM-CX execution tool in this session. Verification
summary lines and log paths: **none; not run**.

**Hash, docs, files and ownership**

New report-only state is named explicitly: pending/active equity report mode,
`summary_equity_fold_`, `summary_equity_snapshot_`, its validity/readiness
flags, `summary_equity_ratios_`, and `summary_broker_equity_prefix_`.
They are outside the broker-state region, like other output/capture state;
no new broker hash leaf/domain/seed or waiver-file entry was introduced.
The ordered point witness is a separate report/replay check. The broker
prefix is reconstructible from the reported points and restores the existing
four hashed extrema; it is not independent decision state. This output-only
classification is explicit for supervisor hash review.

Files touched:
`include/pineforge/engine.hpp`, `include/pineforge/metrics_fold.hpp` (new),
`src/engine_metrics.cpp`, `src/engine_report.cpp`,
`tests/test_metrics_fold.cpp` (new),
`tests/test_summary_metrics_allocations.cpp` (new),
`tests/cr150.d/core-cr150-o1w-metrics.cmake` (new),
`O1W_METRICS_REPORT.md` (new).

Public documentation anchors shifted: none. PF_API declarations,
implementations, symbol inventory/counts and legacy report/POD sizes changed:
none. No dependency added. The contract file was read from the contract
worktree and was not copied or edited here.

Sibling lanes must avoid edits to the new metrics header/tests/CMake lane
file, `BacktestEngine::record_equity_point` and its summary members/helpers,
ordinary `fill_metrics_section` and its equity preparation branch, and the new
`equity_month_key` function. The supervisor must combine the one enum overlap
and coordinate the explicit terminal-helper call above. Selected capture,
envelope/transport, replay and omitted-row producers remain their owners'.
