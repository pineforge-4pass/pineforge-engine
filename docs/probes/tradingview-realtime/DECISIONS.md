# Pre-registered probe decisions

Write the chosen mode and thresholds into `results.md` before the scheduled
start. The signatures below classify only clean runs. Any invalidating sequence
means repeat the run; it is not evidence for either outcome.

## P1 — cross-command public ID

- Both `entry_call` and `raw_order_call` fill at the shared trigger: entry and
  raw-order namespaces coexist for that placement order.
- Only the second call's tag fills, with the result reversing between the two
  modes: last writer replaces across command kinds.
- Only the first call fills in both modes: first writer wins.
- Invalid: timeout/cleanup, a non-probe position, or only one placement mode.

This probe does not re-test exit-vs-entry independence or cancel semantics,
which already have repository evidence; it resolves the remaining entry versus
raw-order collision.

## P2 — replacement priority

`A_initial`, then `B_unchanged`, then `A_reissued` must be observed before a
single market event makes both A and B eligible at the same price.

- `A_reissued` precedes `B_unchanged` in TradingView's alert log on repeated
  clean runs: replacement preserved A's original priority.
- `B_unchanged` precedes `A_reissued`: replacement reset A behind B.
- Invalid: `inconclusive_fill_before_reissue`, different fill prices/times,
  only one sibling filling, or reliance on webhook arrival order alone.

## P3 — stop-limit activation across reissue

Required trace prefix:

```text
stop_limit_placed -> stop_price_observed -> reissued_between_stop_and_limit
```

- Fill at the limit on the same market update represented by
  `limit_observed_before_stop_recross`: activation persisted across the reissue.
  The broker fill alert may precede the script trace because broker processing
  and every-tick script calculation have separate event ordering.
- No fill on that first limit descent, followed by
  `post_reissue_stop_recross_observed` and a later limit fill: activation reset.
- Invalid: reissue not strictly between stop and limit, fill before reissue,
  missing price milestones, or a gap/tick that makes the ordering ambiguous.

Apply this rule separately to unchanged and changed-limit reissues.

## P4 — trailing watermark across reissue

At `trail_reissued_on_discriminating_retrace`, preserve the emitted
`pre_reissue_watermark`, `preserved_stop_candidate`, and
`reset_stop_candidate`.

- Fill when the preserved candidate is crossed but before the reset candidate:
  activation/watermark persisted.
- No fill at the preserved candidate, then fill only when the reset candidate
  is crossed: the reissue reset the watermark.
- If a reset order must reactivate, use
  `post_reissue_reset_activation_tick_observed` as the reset watermark source;
  the creation-close candidate is not authoritative.
- Invalid: `inconclusive_post_reissue_new_high`, fill before reissue, a gap that
  crosses both candidates in one event, or missing candidate values.

Apply this rule separately to unchanged and changed-offset reissues.

## P5 — no-`from_entry` persistence

The initial no-`from_entry` exit must be created while only A is open. B must
then open without reissuing that exit.

- In `persistent_no_reissue`, the exit closes both covered lots; compare its
  quantities and trade rows with `positive_control_reissue_after_B`.
- B remains uncovered despite the price crossing B's applicable exit level,
  as proven by `tv-bars.csv`, while the positive control covers it: the exit
  did not persist.
- Invalid: A exits before B opens, no applicable level is crossed, timeout, or
  the positive control does not produce the expected covered exit.

The current TradingView documentation already states persistence. This is a
version-pinned corroboration/control probe.

## P6 — `from_entry` creation cutoff

Compare all three modes using identical distances:

- `entry_then_exit`: records whether an exit called after its entry in the same
  confirmed calculation covers that pending entry.
- `exit_then_entry`: records whether the reverse source order covers it.
- `exit_then_entry_reissue_after_fill`: positive control for coverage after a
  matching entry exists.

A fill is evidence only after the corresponding profit/loss level is reached.
Use `tv-bars.csv` to prove a crossing for any negative conclusion. Timeout or
absence of a discriminating crossing is inconclusive.

## P7 — simultaneous exit/reversal priority

For each source-order mode, require the initial long fill, the
`collision_orders_created` trace, and fills sharing one fill-bar time.

Record the TradingView alert-log fill order, transaction quantities, closed
trade rows, and final position. The repeated result is the rule; this probe does
not pre-assume exit-first or reversal-first behavior. Different results between
repeats are inconclusive/nondeterministic evidence.

## P8 — OCA same-event cancellation

The current Pine v6 reference states that OCA siblings executing on the same
tick cannot cancel/reduce one another. This probe is version-pinned realtime
corroboration, not an undocumented semantic.

For each source-order mode, require both siblings at the same stop and the same
eligible market event.

- Exactly one of `OCA_A` / `OCA_B` fills: OCA cancellation removes the sibling
  before a second same-event fill.
- Both fill at the same fill-bar time/price: TradingView admits same-event
  multi-fill before cancellation takes effect.
- Invalid: different eligibility events, timeout, or disagreement between
  TradingView alert log and trade export.

## P9 — per-lot stop-type selection

Require two same-ID lots whose actual `strategy.opentrades.entry_price()` values
meet the configured minimum separation. Preserve the setup trace containing
each entry time/price, fixed stop, trail activation, watermark, and trailing
candidate.

Map every exit row back to its entry time/price in the List of Trades. Lot 0 has
quantity 1 and lot 1 has quantity 2, so `strategy.order.contracts` plus the
remaining open quantity identifies the child leg. Use the `fixed_stop_fill`
versus `trailing_stop_fill` tag and the traced candidate path to identify the
selected stop type.

- A full per-lot conclusion requires one natural child fill while the other
  quantity demonstrably remains open, followed by a second natural,
  discriminating child fill. Cleanup cannot establish the remaining lot's
  selected stop type; after cleanup the result is limited to the first mapped
  lot or remains inconclusive. Each observed fill must satisfy only the mapped
  lot's candidate allocation.
- Invalid: `inconclusive_lot_prices_too_close`, no discriminating candidate
  path, timeout, a reload/repaint, or inability to map the exit row to one lot.
