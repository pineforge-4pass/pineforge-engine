# TradingView refuses a gapped default-size stop entry it cannot pay for (lane W10-DIAG-UNKNOWN, rule GAPSTOP)

Under `process_orders_on_close`, a `strategy.entry` stop placed from flat with
v6's default size (`percent_of_equity`, 100) is sized at its level: the
quantity is the equity at placement over the level, floored. A later bar can
OPEN through that level; TradingView then fills the stop at the open. It does
so only when that equity still pays for the quantity at the open; otherwise it
refuses the order, which is gone: a later cross of its level does not fill it.

Each directory is one `lab tv --no-note` export of a synthetic probe written
for this lane (channel `ws-report-v1`, `range proof: covered`), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. All three run on NASDAQ:AAPL 15, 2025-04-01 .. 2025-05-31, with
`pyramiding = 0`, no commission or slippage, fixed `timestamp("UTC", ...)`
cells and a `strategy.close_all()` cleanup.

| tape | configuration | tv_trades.csv sha256 |
|---|---|---|
| `w10-pooc-gap-stop` | `process_orders_on_close`, fixed 1 | `42bd05b872fdffef2a99042c9bf5900c32b424f044844740e5f791ccd8c9e0bb` |
| `w10-pooc-gap-stop-pct` | `process_orders_on_close`, v6 default size | `0721521274b0f8f485b276c5e659f35224183c9d448fdfe3940bf9bee6b71d43` |
| `w10-gap-stop-pct-noclose` | v6 default size | `1bc2e3e6fbf2cdcadbf553dbe72dc93a8cd4364eeb7d6ddc89d2420f8a7ce92c` |

Each cell places a buy stop at a bar's high on its close; the next bar opens
above the level (G1, K1, K2, G2) or crosses it intrabar (X). K1, K2 and X
cancel the entry on the next bar's calculation if the position is still flat,
as a pending-signal script does.

| cell (UTC) | level | open after | `w10-pooc-gap-stop-pct` |
|---|---|---|---|
| G1 04-11 15:45 | 195.66 | 195.67 | equity 100,000 sizes 511; 511 × 195.67 = 99,987.37 is paid: fills 511 @ 195.67 |
| K1 04-15 17:00 | 202.41 | 202.42 | 101,287.72 sizes 500; 101,210.00 is paid: fills 500 @ 202.42 |
| K2 04-22 13:45 | 197.855 (197.86 on the grid) | 197.87 | 101,517.72 sizes 513; 101,507.31 is paid: fills 513 @ 197.87 |
| G2 04-29 15:30 | 211.17 | 211.18 | 102,630.93 sizes 486; 486 × 211.18 = 102,633.48 is not: no fill, then or at the later crosses (16:00, 17:45) |
| X 05-01 14:00 | 211.59 | 210.60 | 102,630.93 sizes 485; fills 485 @ 211.59 at the level |

The fixed-size tape fills G2 (1 @ 211.18), so the refusal is the money, not
the gap; the tape without `process_orders_on_close` fills G2 485 @ 211.18,
which its equity (102,610.54) pays for. The base engine admitted the gapped
G2 of `w10-pooc-gap-stop-pct` and let a one-unit margin call take the excess
(a 1 @ 211.18 row closed on its own bar, then 485 @ 211.18); it now refuses
the order. The population probe that showed it is
`vasudevshenoy-manoj-betrayed-me` on NASDAQ:AAPL 15 (2025-04-22 14:00, where
TradingView's refused entry leaves the position flat and the script cancels
its id); it becomes byte-identical to its tape.

`tests/test_pooc_gap_stop_tapes.cpp` replays the three tapes through the Pine
adapter under the configuration the generated constructor declares, trading
from 2025-04-11 15:45 UTC, with TradingView's 0.01 tick and whole-share lot,
and requires each trade the tape closes inside the replayed bars (entry and
exit time, side, price in ticks, quantity) to be the engine's. It also reads
the rule off TradingView's own rows. Fail-before on 70ea8a37: 107 passed,
1 failed (the `w10-pooc-gap-stop-pct` replay).

`bars.inc` holds the lane 15m chart feed rows 2025-04-10 13:30 .. 2025-05-01
20:45 UTC (sha256 `ae2b03d3736f057cf4072185d637b7c2edd9350d7a1d83ddf0178cbf292279a0`,
derived from the 1m feed `09c2dc4060ee922e960d080bb5bbabb23e339288e1264da7e8392d434a9f0308`).
