# An exit leg beside a competing order under calc_on_order_fills

Under `calc_on_order_fills`, a stop or limit leg of a `strategy.exit` created in a fill recalculation acts
on the rest of the fill bar exactly as it does with no other order in the book. TradingView has no
"competing chart tick" exclusion: whatever else the book holds (an entry order of the other side, a
`strategy.order`, a global exit beside an opposite entry, a second exit of the same entry, two live
partial exits), a level the bar's tick-quantized extreme reaches books on that bar. That holds for
on-grid levels touched or crossed, in either binary spelling of the ladder point (`k / 100` or
`k * syminfo.mintick`), absolute or relative (`profit=` / `loss=`), and for off-grid levels the extreme
reaches, including one between a half-cent raw extreme and its chart tick.

The engine used to move such a leg's trigger half a tick outward when the source book held two or more
distinct (id, from_entry) keys and the level read off the tick grid (`flush_pending_bracket_legs`, the R4
slice C port of the legacy fallback's `pending_orders_.size() == 1` scope gate, which no tape backed). The
leg then booked one bar late or at its re-issue. About one cent level in seven (`k * 0.01 != k / 100`,
e.g. 9.95) read off the grid there too, so even an exact touch of such a level was moved. The shift is
gone: the leg keeps the trigger `exit()` installs, as in a one-exit book.

## The tapes

Synthetic scripts of our own on NYSE:F 15m (mintick 0.01, capital 1,000,000, pyramiding 1, quantity 1;
2 for `k2z`), each exported twice with `lab tv --no-note`, the two exports byte-identical
(`provenance.json`: the pin's script name, range, returned range, raw-frame hashes of both exports, the
predictions file, frozen and hashed before the export, and the codegen that froze `generated.cpp`).
Each script runs a list of events (`events.json`, by the `events` key of its provenance): on the signal
bar B-1 it enters at market when flat, the entry fills at the open of B, the exit `X` at a level LV
chosen from B's own extreme works on B (first placed in the entry's fill recalculation, or as noted
below) and on B+1, and at B+1's close the script cancels it and closes at market (B+2's open). So a
tape row with the exit inside B is a fill on the touch bar. Signal times are a float array (codegen
lowers an int `array.from` to `std::vector<int>`, which truncates 13-digit epoch milliseconds).

A tape is named `<family>-<side>-<leg>-<book>-<calculation>`:

| part | values |
|---|---|
| family | `f`: the exit first called in the entry's fill recalculation, cent extremes; `g`: the exit placed with the entry on B-1 as a bracket and re-called while held; `r`: a relative leg (`profit=` / `loss=` N ticks from a cent open, the extreme exactly N ticks away); `h`: as `f`, on half-cent raw extremes; `w`: a bracket placed with the entry while flat and never re-called; `y` / `z`: the `f` / `h` shapes on a fresh year with new events |
| side, leg | `l-lim` sell limit, `l-stp` sell stop, `s-lim` buy limit, `s-stp` buy stop (entry `L` long or `H` short) |
| book | `k1` the exit alone; `k2o` beside an opposite-side `strategy.entry` stop never reached; `k2r` beside a `strategy.order` stop never reached; `k2g` a global exit (no `from_entry`) beside the opposite entry; `k2x` / `k2y` beside a second full exit of the entry (its other leg, never reached) called after / before `X`; `k2z` two live 50% exits of a 2-unit entry, the never-reached one called first |
| calculation | `cf`: `calc_on_order_fills`; `nc`: on close |
| range | `f`, `g`, `r`, `h`, `w`: 2025-04-01 .. 2026-05-01; `y`, `z`: 2024-04-01 .. 2025-04-01 (`bars/` holds the lane's chart-feed rows of each range) |

The event classes, by LV against B's extreme X (d = +1 for a leg the high reaches, -1 for the low):

| class | level | |
|---|---|---|
| `Tm` / `Tp` / `To` | LV = X, a cent: k * 0.01 != k / 100 written k / 100 (`Tm`) or `k * syminfo.mintick` (`Tp`); read on the grid either way (`To`) | on-grid touch |
| `Cm` / `Co` | a cent the extreme passes by 2 ticks or more, misread / exact | on-grid cross |
| `Ou` / `Ol` | X a cent print, LV = X - d * 0.003 / X - d * 0.007 | off-grid, reached |
| `Hx` / `Hi` | X a half-cent raw print, LV = X + d * 0.002 (only the chart tick reaches it) / X - d * 0.002 | off-grid, sub-tick extreme |
| `Hm` / `Ho` | LV = the outward cent tick X + d * 0.005, misread / exact | on-grid, sub-tick extreme |
| `Rm` / `Ro` | relative leg, the extreme exactly N ticks from a cent open, misread / exact | on-grid, relative |
| `Wm` / `Wo` / `Wt` | a cent LV half a tick beyond a half-cent extreme, misread / exact; a misread touch | the flat bracket |

The 68 tapes are a superset of every cell of the pin's 112 scripts (px-pin-grid, sets 1 to 3; the other
40 repeat these cells in more book shapes, plus the 4 D side scripts): every class on both sides and on limit and stop legs, beside
an opposite entry, a `strategy.order`, a global exit, two live partial exits and a second exit, in two
years, with each competing shape's one-exit control. On TradingView every `k2o`, `k2r`, `k2g` and `k2x`
tape is byte-identical to its `k1` control; `k2z` books two partial exits per event and `k2y` gives its
second full exit nothing (the exit quantity reservation), so those differ by design.

## The test

`test_coof_competing_tick_tapes` (tests/test_coof_competing_tick_tapes.py) runs every tape's frozen
generated strategy through the C ABI and requires the engine to book TradingView's trades row for row
(count, side, quantity, entry and exit time and price). It also checks TradingView's side: the
control pairs above are byte-identical, and the competing `calc_on_order_fills` tapes cover every class
of their family on both sides and both legs. The `calc_on_order_fills` tapes must be refused by a forward
stream; the 8 tapes on close are replayed as a stream over each run of the feed between two nights (1
and 30 bars of history) and must book the trades of a backtest over the same bars.

## Before the change

The engine before this rule (main 7a1f01c0) books 36 of the 68 tapes and departs on the other 32: every
`cf` tape whose book is `k2o` (7), `k2r` (15), `k2g` (2) or `k2z` (8), each from its first misread or
off-grid event on (e.g. `f-l-lim-k2r-cf` trade 1: TradingView exits 9.95 on the 2025-04-01 14:15 UTC
touch bar, the engine two bars later at 9.98). The 30 `k1` tapes, the 2 `k2x` and 2 `k2y` tapes and the 2
`k2o` tapes on close were already right (in `k2x` and `k2y` the exit called second gets no quantity
beside the first, on TradingView and in the engine alike; on close there is no fill recalculation).

## Not here

- `strategy.close(id)` beside a resting opposite-side entry under `calc_on_order_fills`: TradingView closes
  at the next open, the engine never fills the close (the pin's `D` scripts; its own rule, not this one).
- The two other legacy "competing" gates of the same shape, `qualified_recross`'s competing opening and
  `pooc_short_tick_scope`'s competing entry, are not exercised by these tapes.
