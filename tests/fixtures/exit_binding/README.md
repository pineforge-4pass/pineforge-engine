# When TradingView binds a strategy.exit

A `strategy.exit(id, from_entry = X)` binds when the script calls it, not when X fills. The call
looks at X at that moment:
- **X holds open lots:** the exit is bound to those lots and ends with them -- a close, a
  `close_all`, its own fill or a reversal, whether or not the position goes flat. A later fill of X
  never wakes it.
- **X holds no lot but has an entry order working:** the exit is bound to the id X and waits for X's
  next fill. It survives everything before that fill: a `close_all`, a `strategy.close` of X or of a
  sibling, the flat, a reversal, and a `strategy.cancel(X)` followed by a new `strategy.entry(X)`, even
  across a flat. When X fills, the exit binds to that fill; a stop already through there fills at
  once, at the fill with the stop's slippage.
- **Neither:** the call is ignored. It creates nothing, moves no level of an exit that stands for the
  pair, and detaches nothing.

A later call binds again. A global exit (no `from_entry`) binds the same way: to the open position's
lots, else to the entry orders working, else it is ignored.

Two more facts come from the same tapes:
- an entry order of the side a close flattens, resting from an earlier bar, survives that close when
  it is a stop, as a limit does (`stop-entry-parent`);
- an add called while the position already holds `pyramiding` entries is never placed, so it does
  not fill after the position shrinks or goes flat (`pyr1`, `pyr1-close-sibling`).

## The rule parts and their switches

`ExitBindingRuleSwitches` (include/pineforge/source/pine_adapter.hpp) holds one switch per part, all
on. `tests/test_exit_binding_tapes.cpp` turns each off in turn and requires exactly the tapes below to
depart from TradingView, so every part is live and pinned:

| switch | the part | tapes that need it |
|---|---|---|
| `pending_bound_exit_survives_flat` | an exit called in position for an entry id with no lot but a limit or stop order working survives the flat and closes, a cancel of the id's order keeps it for the id's next order, and the id's fill binds it | 16 tapes (the test's list); the nine named `pending-*` tapes of tests/fixtures/global_exit_children |
| `global_exit_binds_working_entries` | a global exit called while flat waits for the fill of the limit and stop entry orders working | `pending-89` of tests/fixtures/global_exit_children |
| `resting_stop_entry_survives_close` | under `process_orders_on_close`, an earlier bar's stop entry survives a close's flat | `stop-entry-parent` |
| `priced_add_at_cap_not_placed` | under `process_orders_on_close`, a priced add of an id holding no lot, still at the pyramiding cap once its bar's closes are done, leaves the book at the next opening, judged at that opening only | `pyr1`, `pyr1-close-sibling` |
| `global_exit_binds_held_position` | a global exit called while a position is held binds to that position: an entry order of the other side working beside it lends it neither its side nor its price basis | the `*-opp-*` tapes of tests/fixtures/cross_side_exit (no tape here has that shape) |

With every switch off the engine books what it booked before the rule: 18 of the 36 tapes depart.

**Scope.** Every tape runs with margin requirements off (`margin_long = margin_short = 0`), so every
part but `global_exit_binds_held_position` (pinned by tests/fixtures/cross_side_exit, whose TradingView
exports bind the same with and without a margin requirement) acts only there: with a margin requirement, margin calls act on the same exits and orders, and no
tape (nor the pin's reference model, which has no margin model) covers that; the engine keeps its
former course. No part acts under `calc_on_order_fills`. The two exit parts act on whole exits at
absolute levels. Every tape's pending parent rests at a level (a limit, or `stop-entry-parent`'s stop),
so the binding to the entry id applies to a limit or stop order of the id; an exit called beside a
market order of its id keeps its former course (tests/fixtures/samebar_pyramiding_entries
`fallback-X8c`). For the same reason a global exit waits only for working limit and stop entries:
TradingView does not fire one called beside a market entry under `process_orders_on_close` on the
next bar (tests/fixtures/exit_queue `w3f05-s11`, `w3bf05-g8`). The stop entry and the cap rule are
pinned under `process_orders_on_close` only, the tapes' setting, and act only there; without it the
engine keeps its former rules (tests/test_close_all_coqueued_entry_l4d).

Inside these gates the parts also change shapes no tape covers, as the pin's reference model predicts
them: a parent on the other side of the open position, and a `strategy.order` parent; a re-entry as a
market order after the cancel, in position or after the flat; a cancel and re-entry on the exit's own
bar; a short-side cap, and a cap above 1. Open edges, which no tape decides:
- the stop part keeps a stop-limit entry as well as a plain stop (only a plain stop is pinned);
- a global exit called flat also waits without `process_orders_on_close` (`pending-89` is
  `process_orders_on_close`), and the first fill of any entry takes it, including an id that had no
  order working at the call;
- an add at the cap that is marketable at its own close fills there before the cap part judges it,
  as it did before this rule (the pin's model never places it);
- an add at the cap whose own bar's close frees the slot stays, as before: the cap part judges an add
  once, at the next opening, and the pin's model rejects it at the call. The test's mechanism case
  `cap-add-judged-once` (engine rows on these bars, not a tape) pins that such an add still rests
  and fills after a later entry refills the cap.

## Layout

These are the pin's 36 synthetic control scripts on BINANCE:ETHUSDT.P 15, 2025-10-30 .. 2025-11-05
(the population member whose first divergence this rule removes). Each directory holds:
- `strategy.pine` and `tv_trades.csv` (times at UTC+8), byte for byte;
- `metrics.json` from the first of two `lab tv --no-note` exports (maintainers' private
  pineforge-workflow, channel `ws-report-v1`, `rangeProof: covered`); the two exports' `tv_trades.csv`
  are byte-identical;
- `provenance.json`: the pin's tape name, both export times, the source and tape sha256, and the
  shared bars file with its sha256.

`bars/` holds the chart feed's bars for the window, from 2025-10-30 00:00 to 2025-11-05 00:00 UTC
inclusive.

`schedules.inc` is the schedule each `strategy.pine` renders, as C++ data, parsed from the script
itself (the pin's restricted grammar: `if time == timestamp(...)` blocks of `strategy.entry`,
`strategy.exit`, `strategy.close`, `strategy.close_all` and `strategy.cancel`); each `strategy.close`
call-site token is the one the C++ codegen emits for that call. `member-replica` is the one script
outside that grammar: it re-calls its exits on every bar with a stop that moves to breakeven on the
first close at or above 1.01 times its first entry's limit, and its per-bar levels are computed from
the same bars and the same logic (`provenance.json` names the bar the breakeven armed on). Never edit
`schedules.inc` by hand.

Every row of every tape must be the engine's: ids, times, side, prices in ticks, quantity, net profit
at the report's precision and commission at ten significant digits.

## Tapes (36)

| fixture | pin tape | trades | what it varies | TradingView |
|---|---|---:|---|---|
| `base` | `xk01-base` | 2 | entry, then exit at 17:15; `close_all` at 20:00 | exit kept |
| `exit-first` | `xk02-exit-first` | 2 | exit called before the entry, same bar | ignored call |
| `exit-earlier-bar` | `xk03-exit-earlier-bar` | 2 | exit at 12:00, entry at 17:15 | ignored call |
| `reissued` | `xk04-reissued` | 2 | exit re-called at 18:00, 19:00 and 20:00, before the close | kept |
| `close-sibling` | `xk05-close-sibling` | 2 | `strategy.close("L1")` instead of `close_all` | kept |
| `exitfill-flat` | `xk06-exitfill-flat` | 2 | flat by the sibling's own exit fill | kept |
| `pyr1` | `xk07-pyr1` | 1 | pyramiding 1 | the add is never placed |
| `nonpooc` | `xk08-nonpooc` | 2 | `process_orders_on_close` off | kept |
| `global` | `xk09-global` | 2 | global exit placed in position | bound to L1's lot, ends with it |
| `entry-after-flat` | `xk10-entry-after-flat` | 2 | exit at 17:15, L2 placed at 22:00 after the flat | ignored call |
| `sameid-after-flat` | `xk11-sameid-after-flat` | 2 | xL1 lot-bound; L1 re-entered after the flat | ends with its lot |
| `sameid-pending` | `xk12-sameid-pending` | 2 | L1 held and a second L1 order working at the call | lot-bound |
| `reversal` | `xk13-reversal` | 3 | a short reverses at 20:00; L2 later fills against it | L2 only reduces the short |
| `close-pending-id` | `xk14-close-pending-id` | 2 | `strategy.close("L2")` while L2 is pending | kept |
| `samebar-before-close` | `xk15-samebar-before-close` | 2 | entry, exit, then `close_all`, one bar | kept |
| `samebar-after-close` | `xk16-samebar-after-close` | 2 | `close_all`, then entry, then exit, one bar | kept |
| `member-replica` | `xk17-member-replica` | 2 | the member's schedule, levels and costs | the member's #66/#67 |
| `pyr1-close-sibling` | `xk18-pyr1-close-sibling` | 1 | pyramiding 1 with `close("L1")` | the add is never placed |
| `nonpooc-reissued` | `xk19-nonpooc-reissued` | 2 | `reissued` with `process_orders_on_close` off | kept |
| `rebind` | `xk20-rebind` | 2 | exit at 12:00 (ignored), re-called at 18:00 while L2 is pending | kept |
| `exit-first-noflat` | `xk25-exit-first-noflat` | 1 | exit, then entry, no position, no flat | ignored call |
| `exit-first-exitfill-flat` | `xk26-exit-first-exitfill-flat` | 2 | exit first; the flat is xL1's fill | ignored call |
| `exit-first-market-pooc` | `xk27-exit-first-market-pooc` | 1 | exit, then a market entry | ignored call |
| `exit-first-market-nonpooc` | `xk28-exit-first-market-nonpooc` | 1 | the same, `process_orders_on_close` off | ignored call |
| `exit-after-market-pooc` | `xk29-exit-after-market-pooc` | 1 | a market entry, then the exit | applied |
| `exit-after-market-nonpooc` | `xk30-exit-after-market-nonpooc` | 1 | the same, `process_orders_on_close` off | applied |
| `exit-first-limit-nonpooc` | `xk31-exit-first-limit-nonpooc` | 1 | `exit-first-noflat` with `process_orders_on_close` off | ignored call |
| `short-member-shape` | `xk32-short-member-shape` | 2 | the short mirror of the member's shape | kept |
| `stop-entry-parent` | `xk33-stop-entry-parent` | 2 | the pending parent is a buy stop | stop entry and exit kept |
| `entry-recalled` | `xk34-entry-recalled` | 2 | L2 re-called with new limits | kept; fills at the last limit |
| `cancel-replace` | `xk35-cancel-replace` | 2 | `strategy.cancel("L2")` at 18:00, L2 placed again at 19:00 | kept: bound to the id |
| `cancel-flat-replace` | `xk36-cancel-flat-replace` | 2 | cancel at 18:00, flat at 20:00 with no order, L2 again at 22:00 | kept |
| `lotbound-noflat` | `xk37-lotbound-noflat` | 4 | L0 + L1 held, a second L1 order and xL1; `close("L1")` leaves a position | ends with L1's records |
| `recall-after-cancel` | `xk38-recall-detaches` | 2 | after a cancel, the exit re-called with nothing working; L2 again | kept: the call is ignored |
| `ignored-call-keeps-levels` | `xk39-ignored-call-keeps-levels` | 2 | as `recall-after-cancel`, the ignored call moving the stop | kept at the first stop |
| `global-exit-first` | `xk41-global-exit-first` | 1 | a global exit called before any order exists | not applied |

The pin tape names are the pin's own; `recall-after-cancel` is renamed because `xk38`'s name states a
hypothesis its tape refuted.
