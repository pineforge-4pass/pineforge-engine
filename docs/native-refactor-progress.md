# Standalone native engine refactor

Status at 2026-09-14: R4-A is complete; R4-B native execution terms are in
implementation. The table records acceptance status, not elapsed work or a
forecast.

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
3. PR #246's historical baseline used `native_order_v2` for request/core/event
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

## Current R4-C epoch baseline

The current L1 baseline uses `native_order_v5`, engine/host
`engine_script_run_v17`, `native-consumer/v7`, broker/stream version 17, and
the source extension domain `pineforge-source-adapter/v2`. The frozen ab9714b
provider remains v16 for the required bidirectional rejection pairs. Identity
values remain `native_order_v1`; native run-spec is v2, calendar remains v2,
the driver is v5, and `PF_ABI_VERSION` remains 4.

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

## Publication and acceptance

Publish reviewed increments in an evolving draft PR so implementation and
open decisions are reviewable. Draft and CI-first publication may precede
the full compatibility sweep. Before merge, require native proof, unchanged
fixed-population compatibility evidence, the actual gate result, independent
review of the final candidate, and green CI. Squash merge and verify the
resulting tree and post-merge checks separately. Never translate a neutral
gate failure into PASS or force baseline promotion.
