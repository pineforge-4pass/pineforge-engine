# A global exit beside an add of the held side

A `strategy.exit` with no `from_entry`, called while a position is held, binds to that position
(tests/fixtures/exit_binding: lots first, then working orders), whatever the side of an entry order
working beside it. tests/fixtures/cross_side_exit pins an entry of the other side; these tapes pin an
add of the HELD side: a limit or a stop that rests and is never reached, or a market add at the
pyramiding cap. TradingView books the exit exactly as if the add were not there: a `profit`, `loss` or
`trail_points` leg resolves against the held position's entry price, and nothing waits for the add's
fill.

The engine used to take a resting same-side add as the exit's parent. With its price basis, a profit or
trail leg rested at the add's limit (1.0 for a long) plus its ticks and filled at the next open, so the
script booked thousands of one-bar trades; a loss leg, and any leg beside a stop add, waited for the
add's fill, which never came, and the position ran to the end of the range. A market add at the cap
never became a pending parent, so those tapes were already right. Under `calc_on_order_fills` one more
path departed: the profit-and-loss bracket the exit stages in the entry's fill recalculation counted the
resting add as a competing order of the book, which took the bracket's chart-tick reach, so a limit the
fill bar's high reaches exactly (9.95 on the 2025-04-01 14:15 UTC bar) filled one bar late. TradingView
has no such competing exclusion beside any order (tests/fixtures/coof_competing_tick), and the engine
has none now.

`ExitBindingRuleSwitches::global_exit_binds_held_position` turns the rule off for tests (the parent, in
`PineExecutionAdapter::exit()`).

## The tapes

Synthetic scripts of our own on NYSE:F 15m (mintick 0.01, quantity 1, capital 1,000,000), each exported
once with `lab tv --no-note` (`provenance.json`: the pin's script name, range, returned range, raw-frame
hash, and the codegen that froze `generated.cpp`). A tape is named `<side>-<leg>-<add>-<calculation>`:

| part | values |
|---|---|
| side | `l`: entry `L` long; `s`: entry `H` short (market, quantity 1, when flat) |
| leg | `strategy.exit("X", ...)` with no `from_entry`, on every held bar: `p` profit = 5, `l` loss = 5, `pl` both, `t` trail_points = 5 with trail_offset = 2 |
| add | `lim`: `strategy.entry("A", <held side>, qty=1, limit=1.0)` (100.0 for a short) before the exit on every held bar, `strategy.cancel("A")` when flat; `stp`: the same with stop 100.0 (1.0 for a short); `cap`: a market add at the pyramiding cap (pyramiding = 1); `ctl`: the control, the script without the add (pyramiding = 2, as `lim` and `stp`); `ctl1`: the control of a `cap` tape (pyramiding = 1) |
| calculation | `nc`: on close, 2025-04-01 .. 2026-05-01; `cf`: `calc_on_order_fills`, 2025-04-01 .. 2025-05-01 |

The 36 add tapes are every cell: 16 `lim` and 16 `stp` (side x leg x calculation) and 4 `cap` (side x
`p`, `pl`, on close). Each add tape's List of Trades is byte-identical to its control's (the pin, before
the export, predicted the control's engine run for both): 36 of 36 pairs. `bars/` holds the lane's
chart-feed rows from the returned range's first bar (the same rows as tests/fixtures/cross_side_exit).

## Known divergences

18 tapes depart from TradingView at one row: 6 controls, with either rule part on or off and on the
engine before this rule, and their 12 add tapes at the same row with the rule on: another rule's
(`known_divergences.json`, with the rows). The trailing leg (`l-t-*-nc` trade 16, `s-t-*-nc` trade 6, `s-t-*-cf` trade 9: the trailing
stop fills on another bar), and `calc_on_order_fills` re-entries in the fill recalculation (`l-l-*-cf`
trade 21 and `l-pl-*-cf` trade 152 on the 2025-04-09 13:30 UTC gap bar, `s-pl-*-cf` trade 28: the
re-entry books at another point of the bar).

## Not modelled: an add that fills

When the add IS reached while the global exit works, TradingView covers both lots, but per entry: each
lot's exit levels come from its own fill price, an entry that fills after the call gets its orders at
its fill, and every exit fill closes the oldest open trade (FIFO). The engine books one exit for the
whole position at the average price. That is a separate rule (pinned by its own synthetics, not
committed here); none of these tapes reaches it, because their adds never fill.

## The test

`test_same_side_exit_tapes` (tests/test_same_side_exit_tapes.py) runs every tape's frozen generated
strategy through the C ABI. With the rule on, every tape not listed in `known_divergences.json`
must equal TradingView row for row and every listed one must depart exactly at its recorded row. With
`global_exit_binds_held_position` off exactly the 32 `lim` and `stp` tapes move, each departing from
TradingView as `rule_off_departures.json` records; no control and no `cap` tape moves.
The 16 `calc_on_order_fills` add tapes must be refused by a forward stream; every other add tape is
replayed as a stream over each run of the feed between two nights (1 and 30 bars of history), and must
book the trades of a backtest over the same bars with the rule on, and over the first run with
`global_exit_binds_held_position` off too.
