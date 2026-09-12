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
| R2 | Resting orders, partial execution, owner-bound children and group effects | Design under review; next implementation slice |
| R3 | Accounting, admission, risk and observation ownership | Open |
| R3a | Reversal execution and lifecycle settlement | Complete, [PR #242](https://github.com/pineforge-4pass/pineforge-engine/pull/242) |
| R3b | Migrate remaining physical fill paths to shared settlement | Open |
| R4 | Complete codegen/adapter policy ownership and native independence | Open |
| R5 | Final requirement and compatibility audit | Open |
| R6 | Resume the parity improvement campaign after the audit | Queued |

These are the nine phases in the campaign ledger, including the queued
return to parity improvement. No R2 design document or partial implementation
marks R2 complete.

## Last accepted implementation

PR #243 was squash merged as
`5f3299d90a02e3161659720fb8c2e82f7543ef7a`. Its tree
`770f7bd2913c7c9c579d38f5c3ffc8950676bb40` is identical to the independently
reviewed and measured candidate `ce995a2db1c3bd771369f105fcce2d1234d107b8`.

- Linux Debug with ASan/UBSan: 317/317 CTests, no skips; direct WebSocket
  check passed. The native proof includes 46 structured scenarios and actual
  runner delivery failure, retry and replay.
- Fixed-population Cloud sweep: 72/72 cases and 4190/4190 scored probes,
  with zero differences in full grades, raw trades, substantive verifier
  results or non-product inputs against the pinned baseline.
- All nine PR checks and four post-merge workflows passed. Independent
  final candidate review was GREEN.
- The actual gate execution `pineforge-pr-gate-7zlr7` returned
  **FAIL: `target.not-positive` only**, because results did not improve.
  This is retained as a FAIL under the authorized neutral-refactor exception.
  The campaign baseline remains unchanged; promotion was skipped.

These results apply to the R1 candidate, not to future R2 edits. The final
receipt is content-addressed in the campaign evidence store as
`7c4487fa35366a2a839f8663223f5f2085e8228ccfae7e772c52acf7f9ff996c`.

## Next increments

1. **Scoped physical closes.** Extend the existing inspection/settlement
   path to close only surviving exposure from a particular opening request
   in an exact position cycle. Whole-book FIFO remains the default.
   This preparatory seam needs its own contract, native checks and review.
2. **Resting-order chronology and partial execution.** Add typed triggers
   and live remainders to the existing driver/consumer path. Distinguish
   actual tick prints from modeled OHLC crossings; protect limit prices.
3. **Causal relationships.** Activate owner-bound children and apply typed
   group cancellation/reduction from committed events, with replacement,
   exhaustion and replay behavior explicit.
4. **Integration proof.** Exercise the real native producer/consumer and
   review the Pine mapping independently before accepting R2.

The first relationship proposal is not accepted. Its review found ambiguous
tick interpolation, remaining-path ordering, same-point child activation,
and missing nonterminal partial executions. These decisions must be resolved
before relationship implementation. Opening-request identity may cover
several physical fragments; it is not a unique lot id. Canceling a working
remainder does not undo its already committed exposure.

R2 preserves all currently supported calendar, session, timezone, DST and
timeframe behavior. Source labels remain inert in the native kernel;
`from_entry`, source call grouping, Pine OCA policy and dormant revival belong
to codegen/adapters. Range-end reporting remains nonphysical.

## Publication and acceptance

Publish reviewed increments in an evolving draft PR so implementation and
open decisions are reviewable. Draft and CI-first publication may precede
the full compatibility sweep. Before merge, require native proof, unchanged
fixed-population compatibility evidence, the actual gate result, independent
review of the final candidate, and green CI. Squash merge and verify the
resulting tree and post-merge checks separately. Never translate a neutral
gate failure into PASS or force baseline promotion.
