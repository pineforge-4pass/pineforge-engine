# What a calc_on_order_fills recalculation reads (lane INT28-FIX, rule RC)

TradingView's own trades for a read-out probe written for this lane: one
`lab tv --no-note` export (channel `ws-report-v1`, `rangeProof` covered), byte
for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01.
`tests/test_coof_recalc_security_tapes.cpp` replays it through the Pine adapter
over the bars in `bars.inc`, the 1m bars as the auxiliary request.security feed.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `int28fix-rc` | calc_on_order_fills, fixed 1 | 2 | `ce254341b35a225d58f7b92a3d1c91a7f37bacd33268cc3d5d3c1a980e260025` |

The probe enters long at the 2025-04-11 00:15 UTC bar and short at the 02:15
bar; each entry fills at the next open, and the recalculation of that fill
closes the position with a comment naming the chart bar's `close`, `high` and
`low`, a 5m `request.security` close and RSI(14), and a 60m close, all with
`lookahead_off`. The close fills at the same open, so each exit row's
`Signal` is what the open fill's recalculation read.

What it shows: the recalculation reads the whole bar. Its close, high and low
are the chart bar's (1520.73, 1523.56, 1514.04 on the 00:30 bar), and the 5m
close is the 5m bar that closes with the chart bar (1520.73), not the 5m bar
before the fill. The engine fed a bar's slice of the auxiliary feed at the
bar's close callback, after its fill recalculations, so the recalculation of the
open fill read the previous chart bar's 5m close (1519.95).

The replay reads the 5m close only: the RSI reads TradingView's whole 5m
history, and the 60m bar the first cell reads closes before the replayed bars.

## Bars

`bars.inc` holds the replayed bars, copied as text from the corpus 15m chart
feed (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`),
2025-04-11 00:00 .. 03:00 UTC, and from the corpus 1m feed it derives from
(sha256 `db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`),
00:00 .. 03:14 UTC; `exec/INT28-FIX-scratch/tools/gen_rc_bars_inc.py` wrote it.
