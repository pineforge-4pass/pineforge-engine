# TradingView's intraday filled-order count (lane W10-DIAG-UNKNOWN, rules CAP-ORDER and CAP-ON)

`strategy.risk.max_intraday_filled_orders(N)` counts the orders that FILL on a
trading day; the order that fills the N-th slot trips the cap, which closes the
position ("Close Position (Max number of filled orders in one day)") and
refuses every later order that day. The Pine cap component
(`include/pineforge/compat/pine/intraday_cap.hpp`) models the count with three
candidate switches, each declared through a `syminfo` metadata key:

- A `intraday_cap_skip_noop_market_fills`: a MARKET entry in the held
  direction that pyramiding makes a no-op is not a fill.
- B `intraday_cap_defer_pooc_close`: under `process_orders_on_close`, the cap's
  close for a same-bar MARKET fill is taken at the next bar's open.
- C `intraday_cap_count_pooc_full_close_fills`: a `strategy.close` that fills
  the whole position is a fill; an opposite MARKET entry co-queued on that bar
  inherits its slot.

Each directory is one `lab tv --no-note` export of a synthetic probe written
for this lane (channel `ws-report-v1`, `rangeProof` covered), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. Both run on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, under
`process_orders_on_close`, a fixed size of 1, `pyramiding = 0` and
`strategy.risk.max_intraday_filled_orders(3)`, with fixed
`timestamp("UTC", ...)` cells and a `strategy.close_all()` cleanup at 06:00.

| tape | days | tv_trades.csv sha256 |
|---|---|---|
| `w10-cap-abc` | A 04-08, B 04-09, C 04-10 | `7782a76c8950668396273e0bfd1191f7a8efcfc0f4dae71ec13d1a0ebc92463d` |
| `w10-cap-rev` | D 04-11, E 04-12, F 04-13 | `b6fd2e0f43a2183515cb44f7345a9e25462c2947ada81b90e3ca9fee9ab37eeb` |

| day | orders (UTC) | TradingView |
|---|---|---|
| A | 00:00 long; 00:15, 00:30, 00:45 the same long again (held: no-ops); 01:00 close; 01:15 a new long | the close is the 2nd fill, the new long the 3rd; the cap closes it at the 01:30 open |
| B | 00:00 long; 00:30 close; 01:00 short | the short is the 3rd fill; the cap closes it at the 01:15 open |
| C | 00:00 long; 01:00 close, then short, on one bar; 02:00 long | close and short are two fills: the short is the 3rd, closed by the cap at the 01:15 open; the 02:00 long never fills |
| D | 00:00 long; 01:00 short, then close of the long, on one bar; 02:00 long | the short reverses the long and the close is void: one fill; the 02:00 long is the 3rd, closed by the cap at the 02:15 open |
| E | 00:00 long; 01:00 short alone; 02:00 long | as D |
| F | 00:00 long; 01:00 close, then short, on one bar; 02:00 long | as C |

Days C and F are rule CAP-ORDER: with the three switches declared, the base
engine let the short placed AFTER the close inherit the close's slot, so the
bar was one fill and the cap tripped at 02:00 instead. The slot now passes
only to an opposite entry placed BEFORE the close (day D), whose reversal is
what closed the position.

Days A, B and C are rule CAP-ON: without any declaration, the base engine
left all three switches off. It charged day A's three held-direction entry
calls, so the cap tripped at 00:30 on the long; it closed each cap trip at the
fill instead of the next open; and it did not charge the closes of days B and
C. A Pine script's own `strategy.risk.max_intraday_filled_orders` statement
now turns on every switch its host did not declare (a declared switch keeps
its value; a component selected any other way keeps its defaults), so an
undeclared run books TradingView's rows. The population probe that showed it is
`officialjackofalltrades-regime-execution-strategy-joat` (process_orders_on_close,
cap 6): XAUUSD 15 and EURUSD 15 become byte-identical to their tapes.

`tests/test_intraday_cap_tv_tapes.cpp` replays both tapes through the Pine
adapter under the configuration the generated constructor declares, trading
from 2025-04-07 23:45 UTC, with TradingView's 0.0001 lot as the `qty_step`,
and requires each trade the tape closes inside the replayed bars (entry and
exit time, side, price in ticks of 0.01, quantity in lots of 0.0001) to be the
engine's, once with the three switches declared and once with nothing
declared. It also reads the count off TradingView's own rows. Fail-before:
CAP-ORDER on c0eaf496 110 passed, 2 failed (the declared runs, days C and F);
CAP-ON on 23840a41 153 passed, 2 failed (the undeclared runs).

`bars.inc` holds the corpus 15m chart feed rows 2025-04-07 00:00 .. 2025-04-13
07:00 UTC (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`,
derived from the corpus 1m feed `db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`).
