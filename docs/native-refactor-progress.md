# Standalone native engine refactor

Status at 2026-09-12: **3 of 9 roadmap phases complete (33%)**. This counts
completed phases; it is not an estimate of elapsed work or remaining time.

The acceptance boundary is a standalone C++ backtest and forward-execution
state machine. Codegen and explicit adapters own PineScript policy. During
this refactor, unchanged compatibility results are acceptable; parity
improvement resumes after the final native audit.

## Completed and remaining work

| Phase | Scope | Status |
| --- | --- | --- |
| R0 | Native settlement and CI convergence | Complete, [PR #241](https://github.com/pineforge-4pass/pineforge-engine/pull/241) |
| R1 | Native market orders, host, drivers, calendar and forward runner | Complete, [PR #243](https://github.com/pineforge-4pass/pineforge-engine/pull/243) |
| R2 | Resting orders, partial execution, owner-bound children and group effects | R2a prerequisite complete ([PR #244](https://github.com/pineforge-4pass/pineforge-engine/pull/244)); resting lifecycle implemented, validation and acceptance pending |
| R3 | Accounting, admission, risk and observation ownership | Open |
| R3a | Reversal execution and lifecycle settlement | Complete, [PR #242](https://github.com/pineforge-4pass/pineforge-engine/pull/242) |
| R3b | Migrate remaining physical fill paths to shared settlement | Open |
| R4 | Complete codegen/adapter policy ownership and native independence | Open |
| R5 | Final requirement and compatibility audit | Open |
| R6 | Resume the parity improvement campaign after the audit | Queued |

These are the nine phases in the campaign ledger, including the queued
return to parity improvement. No R2 design document or partial implementation
marks R2 complete.

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

## Current R2 implementation

This branch implements the pinned native contract. Validation and review
repairs are still in progress:

1. Limit, stop, stop-limit and trailing requests retain trigger state and
   partial remainders. Matching distinguishes actual tick prints from
   continuous modeled OHLC crossings, protects limit prices, and applies
   explicit request capacity at each original input point.
2. Owner-bound children use committed opening events and cycle identity.
   Typed group cancellation/reduction follows committed execution effects;
   replacement and exhaustion finish their dependency cleanup before a
   command returns. One opening request may own several physical fragments.
   Canceling its working remainder does not undo committed exposure.
3. Request/core/event values use `native_order_v2`; identity values remain
   `native_order_v1`. Engine, pending, host and consumer C++ boundaries move
   together to epoch 13, with broker/stream hash version 13. Existing native
   run-spec, calendar/driver value domains and C ABI 4 prefixes remain.

The local Release suite and separate native acceptance checks pass. One
WebSocket transport test is skipped when the selected macOS libcurl lacks
WebSocket support. Native coverage exercises working-order quantities,
scoped ownership and costs, group effects, driver chronology, admission,
replay/reset, and failure prefixes. Independent review findings have
corresponding regression witnesses, including token ownership, event-sized
partial exits, exact deferred-receipt arithmetic and same-cursor execution prices.
The sanitizer ABI control also supplies the historical v12 destructor needed
for its RTTI; this changes only the link-test stub, not runtime behavior.

Final review of the publication candidate, remaining R2 acceptance coverage,
a new fixed-population 4190 comparison, the actual gate and published-head CI
remain required. R2 is not complete. The earlier R2a and separate AUTO-isolation
compatibility results do not substitute for this implementation's proof.

R2 preserves all currently supported calendar, session, timezone, DST and
timeframe behavior. Source labels remain inert in the native kernel;
`from_entry`, source call grouping, Pine OCA policy and dormant revival belong
to codegen/adapters. Range-end reporting remains nonphysical.
The native R2 boundary is pinned; complete Pine lowering remains mandatory
R4 work. The parity-improvement campaign remains paused through the final
native audit.

## Publication and acceptance

Publish reviewed increments in an evolving draft PR so implementation and
open decisions are reviewable. Draft and CI-first publication may precede
the full compatibility sweep. Before merge, require native proof, unchanged
fixed-population compatibility evidence, the actual gate result, independent
review of the final candidate, and green CI. Squash merge and verify the
resulting tree and post-merge checks separately. Never translate a neutral
gate failure into PASS or force baseline promotion.
