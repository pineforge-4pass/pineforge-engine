# A global exit beside a resting entry of the other side

A `strategy.exit` with no `from_entry`, called while a position is held, binds to that position
(tests/fixtures/exit_binding: lots first, then working orders). An entry order of the other side
that rests beside it, or that the script placed earlier in the same calculation, is not its parent:
TradingView books the exit exactly as if that order were not there. Its limit and stop are tested on
the held side (a sell limit is reached when the tick-built high reaches it, a sell stop when the
tick-built low does; mirrored for a short) and a `profit` or `loss` leg resolves against the held
position's entry price.

The engine used to take that pending order as the exit's parent. With its side, the limit and stop
thresholds sat half a tick on the wrong side of the leg that closes the held position: settled at the
source level, outside its trigger, the leg was refused (`MatchRejected`, `InvalidTerms`), withdrawn,
and re-issued at the next calculation, so the exit filled bars late or only on a gapped open; a stop
fired a tick late. With its price basis, a relative leg waited for that order's fill, which never
came, and the position ran to the end of the range. `ExitBindingRuleSwitches::
global_exit_binds_held_position` turns the rule off for tests.

## The tapes

Synthetic scripts of our own on NYSE:F 15m (mintick 0.01, quantity 1, capital 1,000,000, the default
margin requirement of 100 %), each exported once with `lab tv --no-note` (`provenance.json`: range,
returned range, raw-frame hash, the codegen that froze `generated.cpp`). Each `*-opp-*` script holds a position
and, on every bar it is held, first places an entry order of the other side that is never reached
(`S` short stop 1.0, `S` short limit 100.0, or `B` long stop 100.0), then calls `strategy.exit("X")`
with no `from_entry`. Its `*-ctl-*` twin is the same script without that entry; a pair's two tapes are
identical row for row in all 21 pairs exported: these 8, the same 8 with `margin_long = margin_short = 0`
(TradingView's tapes are byte-identical to these), and 5 more whose engine departures are another rule's
(the partial-window warm-up of `ta.lowest` at the range start, and calc_on_order_fills).

| shape | exit leg | calculation | tapes |
|---|---|---|---|
| `ll` | long, limit at `round_to_mintick(ta.highest(high, 10))` of the first held bar | on close | `ll-ctl-nc`, `ll-opp-nc` |
| `ss` | short, stop at the 10-bar high | on close | `ss-ctl-nc`, `ss-opp-nc` |
| `rlp` | long, `profit = 5`, beside a resting short stop / short limit | on close | `rlp-stp-ctl-nc`, `rlp-stp-opp-nc`, `rlp-lim-opp-nc` |
| `rsp` | short, `profit = 5`, beside a resting long stop | on close | `rsp-stp-ctl-nc`, `rsp-stp-opp-nc` |
| `ls` | long, stop at the 10-bar low; entries from 2025-04-02 13:30 UTC | `calc_on_order_fills` | `ls-ctl-cf-w`, `ls-opp-cf-w` |
| `ls`, `sl` | long stop at the 10-bar low / short limit at the 10-bar low; entries from 2025-04-02 13:30 UTC | on close | `ls-ctl-nc-w`, `ls-opp-nc-w`, `sl-ctl-nc-w`, `sl-opp-nc-w` |

`-nc` tapes cover 2025-04-01 .. 2026-05-01, `-cf-w` 2025-04-01 .. 2025-05-01; `bars/` holds the lane's
chart-feed rows from the returned range's first bar. The forward prediction for every tape, written
before its export, was the engine's run of the pair's CONTROL script; it held on every tape here except
the four `known_divergences.json` names.

## Scope and the witness re-pin

The rule acts whatever the margin setting. It moved fills in three random witnesses of
`test_adapter_quiet_bar`, whose table was re-harvested as an approved re-pin of 2026-10-05 (12 of its
276 rows):
- Random27 trade 18 (17 under the magnifier), Random31 trade 5 and Random12 trade 2 are this rule's own
  case: the bar's low or high touches the global exit's stop or limit exactly, the resting opposite entry
  is not reached on that bar, and the exit fills there, where the engine used to miss the level and close
  the position later (through the opposite entry's reversal, or at the exit's other level). Random31
  trade 6 follows from its trade 5: the opposite limit then fills from flat at the size it was placed
  with, its reversal size. TradingView does that on the exit's own bar (`x1-stopbelow`, below); that it
  does so on Random31's later bar, with the entry placed once three bars before, is an inference.
- Random12 trades 4-6 (magnifier off) are a known divergence, below.

## Known divergences

**An exit and an opposite entry reached together.** When the global exit and the resting entry of the
other side are both reached on one bar, TradingView executes both as independent orders (`lab tv`
synthetic controls exported for this rule and not committed here, NYSE:F 15m, long 2 with a global exit
at the 10-bar low and high and an opposite short entry of quantity 1, the entry placed before the exit): where the path reaches the exit first, the exit closes the
long and the entry then opens from flat at the size it was placed with (short 3); where the entry is
reached first, or both at the same level, or a bar gaps through both, the entry reverses first and the
exit's sell of 2 follows, opening a second short (`x1-stopbelow`: short stop 1 tick under the exit's
stop; `x2-limitabove`: short limit 1 tick over its limit; `x3-stopsame`: short stop at the exit's stop).
The engine with this rule books the first case as TradingView does and fills the exit first in the
others; without the rule it refuses the wrong-side exit. Neither books the entry-first rows: the engine
departs from those tapes at trade 21 of 22, 83 of 1131 and 8 of 9 (main at trade 1 of each).

**quiet_bar Random12 trades 4-6** (magnifier off) are recorded as a known divergence by decision. The
rule's skip at that call (bar 14) is caused by the short limit `A` at 96.25, resting since bar 12; the
short stop `S` at the exit's own stop (92.75) is placed after the exit in that calculation, so it is never
the exit's parent. The rule books short 5 in one lot (the exit, then `S` from flat at its reversal size),
the engine without the rule short 2. No tape has this exit-first call order (`x3-stopsame` places the
entry before the exit).

**math.round_to_mintick of a half-cent level.** `ls-*-nc-w` and `sl-*-nc-w` depart at trade 6, the same
row in each pair and with or without the rule: its level is `math.round_to_mintick(ta.lowest(low, 10))`
of a 10-bar low on the half-cent print 9.655 (2025-04-03 17:15 UTC). TradingView rounds it to 9.66, the
engine to 9.65, so the exit on the 18:00 bar fills at 9.66 on the tape and 9.65 in the engine (the same
9.245 -> 9.25 residual as Ki67 #35). Before that row the tapes are equal.

## The test

`test_cross_side_exit_tapes` (tests/test_cross_side_exit_tapes.py) runs every tape's frozen generated
strategy through the C ABI, at the default margin requirement. TradingView's margin-0 exports of the same
scripts are byte-identical to the default-margin ones (the pin's evidence); the committed test runs the
default margin. With the rule on, every tape not listed in `known_divergences.json` must
equal TradingView row for row and every listed one must depart exactly at its recorded row. With the
rule off, exactly the `*-opp-*` tapes move, each departing from TradingView as
`rule_off_departures.json` records, and no control moves. The calc_on_order_fills `opp` tape must be
refused by a forward stream; every other `opp` tape is replayed as a stream (1 and 30 bars of history)
and must book the backtest's trades with the rule on and off.
