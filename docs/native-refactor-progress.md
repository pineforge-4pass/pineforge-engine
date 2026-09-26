# Standalone native engine refactor

Status at 2026-09-14: R4-A is complete; R4-B native execution terms are in
implementation. The table records acceptance status, not elapsed work or a
forecast. That snapshot predates the close of R4 and the R5 audit waves, so
read the table below as its record, not as today's status.

The acceptance boundary is a standalone C++ backtest and forward-execution
state machine. Codegen and explicit adapters own PineScript policy. During
this refactor, unchanged compatibility results are acceptable; parity
improvement resumes after the final native audit.

## Completed and remaining work

| Phase | Scope | Status |
| --- | --- | --- |
| R0–R3a | Native settlement, resting requests, scoped settlement and lifecycle prerequisites | Complete; retained as the native foundation |
| R3b | Shared settlement fill migration | Complete ([PR #247](https://github.com/pineforge-4pass/pineforge-engine/pull/247)) |
| R3 observation | Source-day observation isolation | Complete ([PR #248](https://github.com/pineforge-4pass/pineforge-engine/pull/248)) |
| R4-A | Selected closes, current-point execution, native run specification and forward lifecycle | Complete ([PR #250](https://github.com/pineforge-4pass/pineforge-engine/pull/250)) |
| R4-B | Host-sized terms, exact reversal, precommit view, immutable native FX curve, ABI fencing, example and docs | In implementation; no R4 acceptance claim |
| Slice B | Source-layer cut, including the generic native FX broker-open epoch clock | Intermediate ownership boundary landed; no R4 credit |
| Slice C | Pine compatibility adapter lowering onto the native seams | L3b deletion landing complete locally; pending the campaign-level integration gate |
| R5 | Final requirement and compatibility audit | Open |
| R6 | Resume the parity improvement campaign after the audit | Queued |

The campaign ledger still retains its historical sub-phases. A design document,
partial implementation, example, or test oracle never by itself marks a phase
accepted.

## Accepted scoped-settlement prerequisite

PR #244 completed **R2a scoped native settlement**, the internal prerequisite
for owner-bound closes. It was squash merged as
`e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb`. Its tree
`0201bf052429490fb453bbfd6037e5afd1669626` is identical to the independently
reviewed and measured candidate `794d1f27bf46f23be238f8cdf9173d9a132d3b6b`.
The scope selects all surviving fragments of an opening request in its exact
position cycle, preserving siblings, historical entry costs and one current
execution fee. Existing whole-book settlement remains available.

- Fresh local Release build: 309/309 CTests. The scoped-close suite has
  12 numerical cases and 3319 checks; additive and existing ABI checks passed.
- Completed fixed-population Cloud retry: 72/72 cases and 4190/4190 scored
  probes, with zero full-grade, raw-trade, substantive-verifier, coverage or
  configuration differences against accepted R1. The first 71/72 attempt
  failed during one Cloud task's startup and was abandoned; attempt 2
  completed without a code change.
- All nine PR checks and four post-merge workflows passed. Independent
  final candidate review was GREEN.
- The actual gate execution `pineforge-pr-gate-m6j9h` returned
  **FAIL: `target.not-positive` only**, because results did not improve.
  This is retained as a FAIL under the authorized neutral-refactor exception.
  The campaign baseline remains unchanged; promotion was skipped.

These results apply to R2a. They do not establish acceptance of the later
resting-order implementation, and R2a is not an additional completed roadmap
phase.

## Accepted R2 resting-order state machine

PR #246 completed the native resting-order contract:

1. Limit, stop, stop-limit and trailing requests retain trigger state and
   partial remainders. Matching distinguishes actual tick prints from
   continuous modeled OHLC crossings, protects limit prices, and applies
   explicit request capacity at each original input point.
2. Owner-bound children use committed opening events and cycle identity.
   Typed group cancellation/reduction follows committed execution effects;
   replacement and exhaustion finish their dependency cleanup before a
   command returns. One opening request may own several physical fragments.
   Canceling its working remainder does not undo committed exposure.
3. PR #246's historical baseline used `native_order_v2` for request/core/event <!-- verified HEAD -->
   values; identity values remained `native_order_v1`. Engine, pending, host
   and consumer C++ boundaries were at epoch 13, with broker/stream hash version
   13. Existing native run-spec, calendar/driver value domains and C ABI 4
   prefixes remained.

The local Release suite and separate native acceptance checks pass. One
WebSocket transport test is skipped when the selected macOS libcurl lacks
WebSocket support. Native coverage exercises working-order quantities,
scoped ownership and costs, group effects, driver chronology, admission,
replay/reset, and failure prefixes. Independent review findings have
corresponding regression witnesses, including token ownership, event-sized
partial exits, exact deferred-receipt arithmetic and same-cursor execution prices.
The sanitizer ABI control also supplies the historical v12 destructor needed
for its RTTI; this changes only the link-test stub, not runtime behavior.

The final candidate `c57e8687d91027df09474b414369162e47b94bbb` passed independent
review, 326 local CTests (one macOS libcurl WebSocket skip), all nine PR checks
and four post-merge workflows. It was squash merged as
`e60e57156a54bb1c74c0c0a4f5a338fd924c046d`, with identical tree
`e209c89035ec884774f8f3e68e1dc118f26aaf51`.

The completed Cloud comparison measured 72/72 cases and 4190/4190 probes with
zero grade, raw-trade, substantive-verifier, metadata, configuration or coverage
differences. The actual gate returned **FAIL: `target.not-positive` only**.
That neutral result is retained under the authorized refactor exception;
the baseline stayed unchanged and promotion was skipped.

R2 preserves all currently supported calendar, session, timezone, DST and
timeframe behavior. Source labels remain inert in the native kernel;
`from_entry`, source call grouping, Pine OCA policy and dormant revival belong
to codegen/adapters. Range-end reporting remains nonphysical.
The native R2 boundary is pinned; complete Pine lowering remains mandatory
R4 work. The parity-improvement campaign remains paused through the final
native audit.

## R4-D Slice C native-only source hierarchy

```text
Generated strategy / handwritten Pine fixture
  -> source::PineStrategyHost
       -> NativeStrategyHost
            -> BacktestEngine
Handwritten native host
  -> NativeStrategyHost
       -> BacktestEngine
```

L3b removed the legacy compatibility consumer, default legacy construction,
source pending-order book, source matching loop, lifecycle seams, and the
legacy scheduler/stream bodies after L3a had switched every public Pine route.
`PineStrategyHost` owns a `PineExecutionAdapter` and `PineScheduler`; the
scheduler owns language state by value, and the adapter lowers each source
command into the generic native request state machine. `NativeExecutionConsumer`
is the sole execution owner. The C pending-order ABI remains v1, but its
read-only rows now come from `PendingIntentView`, not a compatibility order
object.

This records a local implementation boundary only. It is not a parity campaign
acceptance claim: the integration owner still performs the final composite
measurement and gate.

## R4-C epoch baseline (historical)

R4-C's L1 baseline used `native_order_v5`, engine/host <!-- verified HEAD -->
`engine_script_run_v17`, `native-consumer/v7`, broker/stream version 17, and <!-- verified HEAD -->
the source extension domain `pineforge-source-adapter/v2`; its native run-spec <!-- verified HEAD -->
was v2. Each of those has moved since: the tree now declares `native_order_v7`,
`engine_script_run_v19`, `native-consumer/v9`, `pineforge-broker-state/v19`,
`pineforge-source-adapter/v4` and `native_run_spec_v3`. Unchanged since R4-C:
identity values `native_order_v1`, the calendar `native_calendar_v2`, the
driver `native_driver_v5`, `PF_ABI_VERSION` 4, and the frozen ab9714b provider
at v16 for the required bidirectional rejection pairs.

## Completed R3 settlement and observation milestones

PR #247 completed R3b shared settlement fill migration. It moved partial,
bound, percent, scratch, reversal, RAW and open/add fill paths onto
the shared native settlement owner while retaining historical paid costs, one
current execution ticket, physical residual quantities, and the established
source-policy boundaries.

PR #248 completed R3 source-day observation isolation. Native settlement and
financial counter checks are independent of source-day state; source
coordinators retain their preflight order and observe only committed rows.

These completed R3 milestones do not make an R4 acceptance claim. R4-B remains
in implementation as recorded above.

## Deferred performance work

The R5 performance wave stopped at the supervisor's cap ("R5 SUPERVISOR RULING
2026-09-25 00:15 Taipei") and the owner's decision ("OWNER DECISION 2026-09-25
00:20 Taipei"), both in the campaign ledger. The items below were measured or
designed and then deferred, and none of them is implemented. Each figure is the
named lane's own, on the host and with the tool given. A ratio is candidate
over base, so below 1 is faster, and "off / on" is the magnifier. Rulings are
quoted by their ledger title. Lane reports and scratch trees sit in the
campaign's evidence tree (`$EV/exec/`). Prototype branches are local to this
repository and were never pushed.

| Item | What it is | Measured size | Deferred by | Prototype and evidence | Reopens when |
| --- | --- | --- | --- | --- | --- |
| D2-B, the quiet-bar protocol and one receipt read per bar | PERF-D2's lane: a per-bar bar-open request, a bulk fold of quiet driver points, the adapter's quiet-open predicate with lazy open bookkeeping, and a gate before the receipt poll. D2-A landed the hook declarations and the O(1) `has_command_after` (`src/native_execution_consumer.hpp:268`). PERF-L4 gated the read inside `observe_terminal_receipts` (`src/source/pine_adapter.cpp:5320`), which still runs about three times a bar | PERF-D2, spark Cortex-A725, callgrind and GBench: about ×0.88 on the 14 probes (an estimate from a measured −13 to −14 % floor bound for the bar-open request alone); after PB the receipt poll still costs about 250 instructions a bar on the no-order floor. On this tree the quiet-bar probe asks the receipt gate 40,996 times and each once-per-bar-open gate 13,248 times: 3.09 reads a bar, 82 % of them stopped at the gate (macOS arm64, AppleClang 17, Release) | "R5 PERF-D2 DONE 2026-09-24 19:56" (D2-B after V19-D); "R5 SUPERVISOR RULING 2026-09-25 00:15 Taipei": "D2-B (quiet-bar protocol) waits for PERF-D3's verdict"; TRIAGE1 §D | Branch `perf-d2/proto-throwaway` (`92e85571`); `$EV/exec/PERF-D2-scratch/patches/`. PERF-D3's S1 (next row) prototyped the request and the lazy opening | A post-release performance epoch. Done when the receipt gate is asked as often as the once-per-bar gates |
| S1a, kernel quiet-bar driving (PERF-D3) | A bar with nothing live, a flat book and a declined opening is driven in a quiet form: the kernel writes the bookkeeping its four price points would leave (ordinals, the last two driver marks, the floor, the point epochs) without walking them or calling the bar-open callback. Additive host cadence declarations; C hosts declare from their callback table; one C symbol under the PF_API checklist | PERF-D3, spark A725, GBench time and `perf stat` instructions, on the L3 tree `b33ea659`, S1a with S1b: no-order floor 0.764 / 0.660 in time (0.718 / 0.656 in instructions), 14 probes 0.958 / 0.921 (0.892 / 0.856), 100 slots 0.972 / 0.956 per slot. A bare kernel host that opts in: idle 0.545× (35 % fewer instructions a bar), market 0.81×, bracket 0.92×. Byte-identical: corpus 312/312, the 132-configuration battery with its hash columns | "FINDING PERF-D3 (filed for a post-release epoch per the owner's cap)" (ledger, 2026-09-24 16:58 UTC); "OWNER DECISION 2026-09-25 00:20 Taipei"; TRIAGE1 §D: "PERF-D3 S1/S2 (post-release epoch)" | Branch `perf-d3/proto-throwaway` (`a820b7df`, five commits on `b33ea659`); `$EV/exec/PERF-D3-scratch/patches/`; spark `~/perf-d3/` | The post-release epoch, with the owner calls PERF-D3 lists (the API under the existing ruling, the restated gate count) |
| S1b, the Pine lazy opening | The Pine host records what its skipped opening and its uncalled input callback would have written, at the bar's first host callback. Member functions only, no layout change | Measured with S1a. A quiet bar then asks 15 gates, not 30, so the quiet-bar cost check `CHECK(asked >= 20 * o.boundary.size())` (`tests/test_adapter_quiet_bar.cpp:1029`) must be restated | As S1a | As S1a. The in-script hash witnesses caught the prototype's one miss (`awaiting_legacy_script_open_ms_`) | As S1a |
| S1c, the held-position wake band | S1 for bars that hold a position with nothing live, 41 % of the median public slot's bars. The host declares a price band, and the kernel calls the opening only when the bar's range leaves it. The adapter derives the band from its margin model and declines when it cannot bound it | PERF-D3, a measurement-only upper bound without the band, which is not sound in general: 100 slots 0.776 / 0.716 per slot, 14 probes 0.830 / 0.761, cumulative against L3 with S1, S2 and D2-A | As S1a; the owner call "Fund the price band" | `$EV/exec/PERF-D3-scratch/patches/hold-ub-measurement-only.py` | As S1a. Size L, parity risk high: TradingView's margin money rules |
| S2 and S2e, the codegen feature manifest | Codegen states which per-bar hooks and publications a strategy can reach, and the engine skips the others; no manifest means every feature. A median 39 of 52 are statically unreachable, over the 312 corpus strategies and the 100 slots alike. S2e adds two gates that stop the Pine risk ledgers of a strategy with no risk rule | PERF-D3, on top of S1: floor −14 % / −15 % in time (−6.4 % in instructions), 14 probes 0.943 / 0.934, 100 slots 0.942 / 0.945. The nine S2 gates are byte-identical. S2e adds about 2–3 % and moves the adapter hash domain `pineforge-source-adapter/v4` to its next version | As S1a; the supervisor's cap: "any structural S1/S2 redesign is a post-release epoch" | In `a820b7df` (a regex-based manifest scanner); `$EV/exec/PERF-D3-scratch/patches/` | A codegen and engine co-change with a corpus re-emission; S2e also needs an owner decision on the hash-domain step |
| The kernel quiet-run loop and the Pine quiet calculation | A loop for consecutive quiet bars of a pumped batch at the chart's own timeframe (label and gap checks in the batch preflight, precomputed intervals and session facts, one calculation frame); and skipping a flat bar's post-script hooks when its script issued nothing | Estimated, not prototyped (PERF-D3's report, section 3.7): about 1,000 instructions a bar for the loop and 800 for the quiet calculation, taking the floor from 3,726 instructions (S1, S2 and D2-A) to about 1,900, or 180–200 ns a bar | As S1a (PERF-D3's follow-up row) | The PERF-D3 report only | After S1a and S1b |
| PGO | Profile-guided optimisation of the library objects: an instrumented build, a training run over the bench strategies, a `-fprofile-use` rebuild and the identity gates, per compiler and target, with a staleness gate on the profile | LAYOUT1, spark A725, GCC 13.3, `perf stat`, trained on 86 slots and timed on the 14 held-out probes: 14.5 % fewer cycles, CPU time 0.865, 7 % fewer instructions, 26 % fewer L1i refills, 36 % fewer front-end stalls, layout spread 9.0 % → 5.1 %, fingerprints identical | "R5 LAYOUT1 DONE + ACCEPTED 2026-09-24 21:45 Taipei": "PGO APPROVED as a lane (PGO1) scheduled LAST in the perf wave after the code lanes (profile staleness)"; the supervisor's cap: "PGO folds into the release lane"; the owner: "release with PGO in the release build"; TRIAGE1 §D | `$EV/exec/LAYOUT1-scratch/`; spark `~/layout1/results/`. The tree carries only LAYOUT1's `-falign-functions=64` | The release lane. BENCH4 timed the standard Release build without PGO, and neither BENCH4's nor REL10-ENGINE's brief names it |
| Compact journal records and a history ring buffer (L3) | Smaller command records and a ring buffer for the request core's history. `WorkingRequestCore::history` (`include/pineforge/native_order.hpp:1709`) hands out the whole vector and the core holds `history_` (`include/pineforge/native_order.hpp:2197`) inside `native_order_v7`, so both are a layout and API change | L3, spark A725, `perf` call graphs, inclusive shares: at most 3.68 % of bracket, 11.99 % of re-issue, 4.26 % of market and 3.42 % of the 14 probes (retiring the history alone: 1.64, 4.51, 2.53 and 1.00 %); only part is realisable, since records must still be written | "R5 L3 DONE + ACCEPTED 2026-09-24 22:10 Taipei", ruling 1: "DEFERRED to the PERF-D3 structural plan"; TRIAGE1 §D: "compact journal records" | `r5/l3` (`b33ea659`, merged by INT22); `$EV/exec/L3-scratch/`; spark `~/l3/prof` | A `native_order` layout epoch |
| A checked-values handoff for executions (L3) | `execution_values` runs twice, in `check_execution` (`src/native_order.cpp:4791`) and again in `apply_execution` (`src/native_order.cpp:4805`); handing the checked values across needs a public type, an API addition with no layout change | L3, as above: 2.21 % of bracket for the two together, about 1.1 % recoverable | L3 ruling 2: "a public checked-values handoff for executions (~1.1% bracket) -> DEFERRED" | As above | An owner decision on the API addition |
| A chunked `trades_` store (L3) | Closed trades in chunks rather than one vector; `trades_` (`include/pineforge/engine.hpp:546`) is protected `BacktestEngine` state, so script ABI | L3, as above: on bracket, vector growth 0.64 %, `Trade` moves 0.74 % and `record_close_trade` 0.43 % (at most about 1.4 %); at most 0.35 % on the 14 probes | L3 ruling 4: "chunked trades_ store (ABI) -> DEFERRED" | As above | An `engine_script_run` epoch that changes the engine's protected layout |
| A zoned chart-day memo (D2-D finding 4) | On a chart with a timezone, `libc_chart_day_key` (`src/source/pine_adapter.cpp:13561`) takes the process timezone lock through `ScopedTimezone` and calls `localtime_r` on every bar, because `on_bar_open` (`src/source/pine_adapter.cpp:15986`) reads `chart_day_key` (`src/source/pine_adapter.cpp:16142`) unconditionally. The non-UTC monthly Sharpe/Sortino walk, `month_key_local` (`src/engine_metrics.cpp:33`), pays one `localtime_r` per equity point. UTC charts use D2-D's memo, `civil_chart_day_key` (`src/source/pine_adapter.cpp:13545`) | Not measured on a zoned chart. The UTC path cost 134 instructions a bar before D2-D's memo and 68 after (D2-D, spark, callgrind on the floor). PERF-K1: `ScopedTimezone` copies the zone name, which allocates for names of 16 or more characters under libstdc++ | TRIAGE1 §D: "D2-D finding 4 (the timezone `localtime_r` memo)", under the owner's cap. D2-D's report: an exact zoned memo "would need the zone's transition table, which belongs in `timezone.cpp`" | None; the UTC memo's witness is `tests/test_chart_day_memo.cpp` | A lane that owns `src/timezone.cpp` and builds an exact zone-transition memo (PERF-K1's Sitka case shows why a shortcut is not exact), byte-identical, with the zoned keys of `test_chart_day_memo` still equal to the computed ones |
| Thread-local reads that remain per call | D2-C moved the three scopes' writes onto the pump's block, `pump_ambient` (`src/native_execution_consumer.hpp:443`). A reader still reaches the block through `tl_runtime_ambient` (`src/ta_extremes_volume.cpp:29`): `bar_context` (`src/ta_extremes_volume.cpp:82`), read by every `ExtremeRing::update` (`src/ta_extremes_volume.cpp:117`), which backs `ta.highest`, `ta.lowest`, `ta.highestbars` and `ta.lowestbars` and, through an embedded `Highest` and `Lowest`, `ta.stoch`, `ta.wpr` and `ta.range`; and `matching_day_partition` (`src/timeframe.cpp:559`), read by the default-anchored `ta.vwap` through `session_day_index`, by `crosses_boundary` on a DAY period (`timeframe.change("D")`) and by the session-period helpers. `decompose_ms_local` (`src/session_time.cpp:248`) reads two thread-locals per call on a zoned clock, and the prepared order storage's `free_blocks` (`src/native_order.cpp:605`) one per prepared mutation. `ema_na_warmup_flag` is read once per EMA, at its first compute | One thread-local access per reader call. AUDIT4, macOS arm64, a dlopen'd tutorial-MACD probe, counted by `_tlv_get_addr` breakpoints and by descriptor patching: 1.000 a bar for one `ta::Highest` and for one default `ta::VWAP`, 1.999 for one `pine_hour` on a New York clock, 0.000 on the no-order floor and with orders. On Linux a TLS-descriptor call costs about 20 instructions: D2-C's 13 → 4 calls a bar was 260 → 80 instructions (spark aarch64, GCC 13, callgrind). INT23: 2.00 a bar on the zoned probe 064, 0.08 on probe 008 | None for the TA and day-partition readers: "RULING: INT23 ACCEPTED" (ledger, 2026-09-24 23:30 UTC) reads "dlopen TLS 13->0/bar", and INT23's report names only the timezone and order-core residuals. This row is their record | `$EV/exec/AUDIT4-opus-scratch/perf/tls/` | A byte-identical lane that hands these readers the pump's block without a thread-local read. `ta::bar_context()` is script ABI, so its public declaration stays as it is |
| "audit-off mode", "LTO", "data-oriented layout" | Named in "OWNER DECISION 2026-09-25 00:20 Taipei" among the post-release structural items: "S1 bulk quiet bars, S2 per-strategy specialization, audit-off mode, LTO+PGO, data-oriented layout". No engine or campaign document defines, designs or measures the three | None | The owner decision | None | Struck from this inventory until a design note defines one of them |

### Recorded witness drift

**PERF-K24's pre-size is dormant under Window retention.** PERF-K24's report
(finding 1) says of the Pine replay that "most of the replay gain comes from"
K4, the batch log pre-size. Since V19-B's step 6 (`08e71bae`) a run under the
default `NativeEventRetention::Window` keeps no driver point and retires its
journal at every script-bar boundary, so K4 has nothing to size.
`reserve_driver_log` (`src/native_execution_consumer.cpp:2585-2590`) returns
unless the retention is Full, and `presize_logs`
(`src/native_execution_consumer.cpp:9103-9114`) sizes no journal under Window.
`NativeRunSpec::event_retention` (`include/pineforge/native_run_spec.hpp:641`)
defaults to Window, and the Pine adapter declares
`NativeEventRetention::Window` (`src/source/pine_adapter.cpp:2322`). K4 still
sizes Full runs (driver log and journal) and Commands runs (journal), and its
witness, `tests/test_native_batch_log_presize.cpp`, runs under Full. PERF-K24's
replay figure (14.9 % less CPU on spark) describes the tree before V19-B. In
AUDIT4's probe an idle Window run holds 0 driver points and no block of 1 MiB
or more, and the same run under Full holds 66,000 points in one block of
7,509,264 bytes.

**PERF-L4 reads the receipts about three times a bar, not once.** PERF-L4's
brief asked it to "read the receipts ONCE per bar instead of three times". The
lane gated the read instead: `observe_terminal_receipts`
(`src/source/pine_adapter.cpp:5320`) returns at its receipt gate when no
command lies above its cursor. `every_gate_is_taken_both_ways`
(`tests/test_adapter_quiet_bar.cpp:1036`) prints
`gate ReceiptRead asked 40996 quiet 33552 live 7444`, against `asked 13248`
for every gate asked once per bar open, so the adapter reads 3.09 times a bar
and 82 % of those reads stop at the gate (macOS arm64, this tree). PERF-L4's
report does not list the narrowing as a deviation. The once-per-bar read
belongs to the deferred D2-B above.

**D2-C's carry witness prints counts that moved at `dbcca850`.**
`rewritten_quotes_carry_alike` (`tests/test_native_settlement_carry.cpp:281`)
prints `rewriting hosts: 12 configurations, <n> fee inputs or rates rewritten,
<m> settlements carried across them`. The row checks only that a carried run
and a re-staged run agree, so both counts are printed, not pinned. D2-C and
INT23 recorded 479 and 397. B-ADAPTER's item 5, `dbcca850` ("a current
execution converts at its cursor's rate, threaded; it neither writes nor
restores the host's clock"), moved them to 489 and 412, and K-ULP1's
`deb344b1` did not. Bisected on macOS arm64 with AppleClang 17, Release: 479
and 397 at `6f4ca1a9`, `deb344b1` and `4b00da92`; 489 and 412 at `dbcca850`,
`91d65ad6` and later. On x86-64 GCC 13 the row printed 491 and 407, and its
`l2 hosts` line 1066 of 1308 where the Mac reads 1003 of 1249, because its
fixture drew twice inside one argument list at two call sites and GCC on
x86-64 evaluates such arguments in the other order. R5 lane H-DOCGATES took
those draws one per statement, left to right, and `scripts/check_rng_draw_order.py`
keeps the class out: x86-64 GCC 13 (Docker `xplat1-gcc13:amd64`) now prints the
Mac's 489 and 412 and 1003 of 1249, and no Mac output moved.

### Scheduled for the next script-ABI epoch

- **PERF-L4's retired reader slots.** `event_high_water_reader_` and
  `terminal_receipt_high_water_reader_`
  (`include/pineforge/source/pine_adapter.hpp:1923-1924`), with their type
  `ReceiptHighWaterReader`, are storage nothing reads or writes since PERF-L4:
  they keep `PineExecutionAdapter`'s layout, which is part of
  `PineStrategyHost`'s and so of the generated script's ABI. They go at the next
  `engine_script_run` epoch, with their two rows in
  `scripts/broker_state_hash_waivers.txt`, which say so.
- **The dead stream seam.** `BacktestEngine::source_stream_entry_comment`
  (`include/pineforge/engine.hpp`) has two implementations, the kernel's
  default and the Pine host's override, and both do nothing. It is a virtual
  of the class every generated strategy derives from, so deleting it moves the
  vtable of every generated script. It goes at the next `engine_script_run`
  epoch (ADR-0001, the row "`source_stream_entry_comment`").

## Publication and acceptance

Publish reviewed increments in an evolving draft PR so implementation and
open decisions are reviewable. Draft and CI-first publication may precede
the full compatibility sweep. Before merge, require native proof, unchanged
fixed-population compatibility evidence, the actual gate result, independent
review of the final candidate, and successful `pineforge/verify` and
`pineforge/parity` statuses on the exact PR head. GitHub Actions CI is an
advisory signal. Squash merge and verify the
resulting tree and post-merge checks separately. Never translate a neutral
gate failure into PASS or force baseline promotion.
