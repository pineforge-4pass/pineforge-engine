Round 2 instrumentation is prepared as an uncommitted diff on
`27fcad938ee10d4b2e09614a423e24ca172fc00a` (`o1w/tail`).
**UNVERIFIED: no build, test, repository script, corpus run, or lab job was
executed by this lane.** No timing, work count, completion, or parity result
below is represented as an observed execution result. Nothing was staged,
committed, or pushed.

Local `git rev-parse 'HEAD^{tree}'` reports
`db1ffedf2b06b05ea570c8d6ee9e3e0a6fa51ad4`. The brief cites
`19a8fcec3e` for the prior lab proof's tree. These identifiers were not
reconciled here; the supervisor must bind the next job to the actual submitted
tree and must not reuse the prior proof as verification of this diff.

The applied change observes R11 only. The predicates, certification decisions,
floating-point expressions, fold order, barrier comparisons, trade output, and
hash leaves retain their round-1 behavior. Batch-B already supplied L18's
spill/backward reader; round 1 supplied the guarded summaries and six cumulative
work counters. No other inventory row is changed here.

**Counters and increment sites.**

`PineExecutionAdapter::PositionTailWork` is the snapshot type.
`position_tail_work()` remains the accessor. Counters are cumulative within
each adapter run. The test observer takes differences on every query and
samples again at run/report phase boundaries to include updates after the last
query.

| Field | Increment site and meaning |
|---|---|
| `queries` | Existing increment at entry to `PositionTailSummary::ancestry` / `margin`; actual qualifying queries. |
| `certified_queries`, `fallback_queries` | Existing mutually exclusive increments in `finish`; actual guard verdict. Replay itself remains in the adapter after a false verdict. |
| `update_rows` | Existing increment after a successful source read in `begin_position` and `observe`, including a row that trips the barrier/order check. |
| `replay_rows` | Existing increment after each successful resident read or spill `previous` in `replay_position_tail`, including a terminating barrier row. |
| `origin_visits` | Existing increment in the ancestry selection callback, before lookup/deduplication. Counts walked live-map memberships, including duplicate memberships and attempts in a query that ultimately falls back. |
| `host_rows` | New increment beside each summary source read and resident replay read. Adapter source callbacks invoke `host.get_trade`; oracle reads are excluded. |
| `spill_rows` | New increment after successful `spill->previous` in production ordered replay. |
| `replay_host_rows` | New increment beside the resident replay read, separating replay from summary-maintenance host reads. |
| `begin_updates`, `begin_rows` | Calls to `begin_position`, and successful reads in its opening-barrier rebuild. |
| `closed_updates` | Successful newly published closed-row reads in `observe`, including a row that invalidates the summary. |
| `open_updates` | Calls to `open_origin`, including attempts that return because the cache is unsafe/inactive. Both applied-entry and opening rebuild callers select the observation policy. |
| `reissue_updates` | Visits to `on_applied`'s existing early synchronization seam, before nested relative/prearmed reissue and before the normal close debit. This counts synchronization opportunities, not emitted bracket legs. |
| `reissue_rows` | Difference in actual `update_rows` across that early synchronization call. These rows are also included in `closed_updates`; this is attribution, not an additional population. |
| `sync_updates` | Calls to `PositionTailSummary::observe`, including cheap no-ops. Existing query, close-consumption, pre-seal, and early applied-event synchronization callers all reach this method. |
| `invalidations` | Calls to `invalidate`, including repeated invalidation of an already unsafe cache. |
| `guard_reason` | Last query's first rejecting condition, set in `finish`; `None` for a certified query. |
| `guard_reasons[reason]` | One measured increment per query in `finish`, including the `None` bin for certification. |

There is no row/origin collection for diagnostics. Query deltas and run totals
are fixed structs in the test reporter. The existing 16-byte certificate and
32-byte origin payload remain the same size; the diagnostic rejection byte uses
certificate padding. A new static assertion also pins the margin disk value to
its existing 32 bytes. These tags describe derived cache failures, not broker
state; the existing `position_tail_summary_` hash waiver already covers
diagnostic counters. No waiver or state-hash file changed.

**Guard reasons and their sources.**

- Lifecycle/publication: inactive cache; explicit invalidation (including the
  existing forced-replay control); publication rewind; missed sealed prefix;
  newly observed row before the barrier; nonmonotone row times.
- Membership: missing older origin; reactivated retired origin; side mismatch.
- Arithmetic: the existing nonfinite-or-negative/sign-bit quantity rejection;
  quantum shift of at least 53 bits; shifted magnitude exceeding the existing
  numerator bound; combined numerator overflow; invalid input certificate;
  nonfinite reconstructed result.
- Adapter invalidation seams: non-Pine host, short-seed incarnation rewrite,
  phase permutation/amendment, intraday-loss relabel, flat reset, and rebind.

`PositionTailRejection` preserves the first reason, so a later
invalid-certificate check cannot hide an earlier shift/overflow failure.
An invalid origin or margin summary carries its original cause through to
selection. Existing invalidation decisions and arithmetic checks are unchanged.
The enabled shift diagnostic distinguishes the two terms of the existing
short-circuit rejection without changing the shift or sum expressions.

**Hook and default-off path.**

`PINEFORGE_POSITION_TAIL_DIAGNOSTICS` selects the diagnostic policy at the
existing adapter-TU test/observe seam. Its definition in `pine_adapter.cpp`
selects `PositionTailDiagnostics` when `PINEFORGE_POSITION_TAIL_OBSERVE` is
present and otherwise defaults to `PositionTailNoDiagnostics`; every helper
also defaults to the latter policy. The default rejection object is empty and
its methods do nothing.
Additional counter writes, reason comparisons, and the applied-event counter
snapshot are inside `if constexpr (Diagnostics::enabled)`. The disabled
`on_applied` branch makes the original single synchronization call.
There is no runtime enable-flag check, observer dispatch, timestamp read,
formatting, allocation, or atomic introduced on the production query path.
The six pre-existing round-1 counters remain as before.

`test_position_tail_scale_out_100k.cpp` installs its reporter and includes the
existing summary-scale driver, whose actual observer selects the counting
policy. Both observer TUs enable the counters in release as well as debug:
no `NDEBUG`, fixture-size, quantity, trade-ID, or expected-population check
enables them. Only the default production policy omits the added diagnostic
operations. The active counters measure actual source reads, memberships,
and guard branches for every input reaching them.
No disabled-path instruction-count or timing measurement was possible here;
the default-off evidence above is a source-path audit.

The reporter is called by `PINEFORGE_POSITION_TAIL_WORK_REPORT` from the
existing `PINEFORGE_POSITION_TAIL_OBSERVE` callback. Formatting and clock reads
live in the test reporter, outside the summary/replay loops. The small shared
`PINEFORGE_POSITION_TAIL_WORK_PHASE` seam samples the adapter at run, report,
and sink-comparison boundaries. Other executables leave this phase hook empty.

**Periodic format.**

Every 4096 observed queries across the executable, the first query of each
run, every phase boundary, and the final `end` or `failed` record produce one
flushed stderr line. This is a format description, not sample execution output:

```text
PF_TAIL_WORK phase=<phase> run=<run> query=<cumulative query index> run_query=<run index> elapsed_ms=<steady-clock elapsed> rows_visited=<replay rows> origins_walked=<live memberships> guard=<certified|fallback|unqueried> reason=<reason> certified=<count> fallback=<count> source_host=<rows> source_spill=<rows> replay_host=<rows> update_rows=<rows> begin_updates=<calls> begin_rows=<rows> closed_updates=<rows> open_updates=<calls> reissue_updates=<calls> reissue_rows=<rows> sync_updates=<calls> invalidations=<calls> q_rows=<delta> q_origins=<delta> q_host=<delta> q_spill=<delta> q_closed=<delta> q_open=<delta> q_reissue=<delta> guard_<reason>=<count> ...
```

All unprefixed counts are cumulative across runs and therefore nondecreasing;
`run_query` restarts for each run. `q_*` values describe the latest completed
query's measured delta since the preceding sample, including intervening
closed/open/reissue maintenance. They may decrease between lines. Phase
samples capture late updates without overwriting the last query's verdict or
delta. They can show that time is being spent in execution, report building,
or sink comparison after tail-query progress has stopped.

The reporter reconciles independent measured fields on each query:
`host_rows == update_rows + replay_host_rows`,
`replay_rows == replay_host_rows + spill_rows`,
`update_rows == begin_rows + closed_updates`, and the guard histogram sums to
`queries`. Its verdict must agree with the actual `certified` argument. It
also rejects decreasing counters within a run. None of these checks derives a
work count from the requested fixture population.

**Assertions and next lab evidence.**

The standalone 100k executable now reuses the existing R11 summary-scale
driver. This adds its small forced-replay/oracle controls before the original
large differential, and preserves every existing N² witness, linear-work
assertion, query-population assertion, T/H/E comparison, metric memcmp, receipt
check, and resident-bound check. The shared driver only gains callback/entry
point seams and an explicit success return needed when its main is included
under another name. No existing assertion was edited, skipped, or weakened.

The next supervisor lab job must:

1. Build the changed test and library targets from this exact diff and finish
   `test_position_tail_scale_out_100k` successfully, retaining its stderr log.
2. Show advancing `PF_TAIL_WORK` records and nondecreasing cumulative
   row/origin/source counts, including a final `phase=end`. Initial replay
   controls intentionally increase fallback and replay totals. Compare
   differences within the later 100k runs; do not mistake those earlier
   controls' cumulative counts for a fallback in the large certified fixture.
3. Show the unchanged R11 control/linear assertions and L18 differential
   comparisons passing. If elapsed time grows while replay stays flat and
   origin/update work grows linearly, the long phase is outside a quadratic
   tail replay. If replay grows, the reason bins and source split identify the
   fallback and its source.
4. Keep `test_position_tail_summary`, `test_position_tail_summary_scale`,
   `test_position_tail_spill`, and `test_position_tail_scale_out` green.
5. Establish zero trade/metric/hash/equity differences against pre-round-2
   behavior, followed by the required full verification/parity process.
   Sink-on/off comparison alone does not establish cross-revision parity.

I could not measure any counter, elapsed time, completion time, CPU cost,
disabled-hook overhead, memory usage, fail-before diagnostic, or output diff.
No verification summary or execution-log path exists from this lane.
`git diff --check` passed during final review (exit 0). A separate
`git diff --no-index --check -- /dev/null REPORT2.md` produced no whitespace
diagnostics; its exit was 1 for the new-file difference. These are static Git
checks, not build or runtime evidence.

**Exact changed files.**

- `include/pineforge/source/pine_adapter.hpp`: diagnostic enum and snapshot fields.
- `src/source/pine_position_tail_summary.hpp`: measured read/update/reason sites.
- `src/source/pine_adapter.cpp`: policy threading and existing invalidation/open/reissue seams.
- `tests/test_position_tail_scale_out_100k.cpp`: enabled observer and periodic reporter.
- `tests/test_position_tail_summary_scale.cpp`: small shared observer/main seams.
- `tests/test_position_tail_scale_out.cpp`: small shared phase sampling seam.
- `REPORT2.md`: this report and the separate proposal below.

The tracked working diff's final `git diff --stat` summary is six files,
459 insertions and 58 deletions, plus this untracked report. HEAD has not
advanced, so a base-to-HEAD diff would not describe these uncommitted edits.

Protect these named adapter/helper/test seams during integration. No C ABI
exports, dependency, documentation anchor/count, CMake registration, measured
kernel source, or state-hash implementation changed.

**Separate second change: unapplied proposal only.**

`open_origin` currently does `origins_.find` followed by `origins_.emplace`
for each new certified origin. C++17 `try_emplace` can avoid that redundant
lookup and the temporary row copy. This concerns summary updates, not the
already single-lookup ancestry query. It makes no claim to solve the observed
hours of runtime. The following hunk is intentionally **not applied** to the
instrumented tree; it is a separate change for supervisor consideration.

Exactness argument: on insertion the same default `Origin` receives the same
side, validity comparison, and diagnostic reason. On an existing key,
`try_emplace` leaves the row untouched, and the same side-mismatch branch runs.
Incarnation keys, generation marks, completed quantities, source reads,
arithmetic, certification, and fallback order do not change.

```diff
Subject: [PROPOSAL 2/2] Avoid duplicate lookup when opening a summary origin

diff --git a/src/source/pine_position_tail_summary.hpp b/src/source/pine_position_tail_summary.hpp
--- a/src/source/pine_position_tail_summary.hpp
+++ b/src/source/pine_position_tail_summary.hpp
@@ -239,19 +239,19 @@
         if constexpr (Diagnostics::enabled) ++work.open_updates;
         if (!safe_ || !active_) return;
-        auto found = origins_.find(incarnation);
-        if (found == origins_.end()) {
-            Origin row;
+        const auto inserted = origins_.try_emplace(incarnation);
+        const auto found = inserted.first;
+        if (inserted.second) {
+            auto& row = found->second;
             row.is_long = is_long;
             // A genuinely new incarnation cannot have earlier completed
             // rows. An old/reactivated incarnation whose subtotal was
             // retired is uncertain and must replay instead of starting at 0.
             row.completed.valid = incarnation > high_closed_origin_;
             if constexpr (Diagnostics::enabled)
                 if (!row.completed.valid)
                     row.completed.reason = PositionTailGuard::ReactivatedOrigin;
-            origins_.emplace(incarnation, row);
         } else if (found->second.is_long != is_long) {
             if constexpr (Diagnostics::enabled)
                 if (found->second.completed.reason == PositionTailGuard::None)
                     found->second.completed.reason = PositionTailGuard::SideMismatch;
             found->second.completed.valid = false;
```
