# What a calc_on_order_fills recalculation reads (lane INT28-FIX, rule RC)

TradingView's own trades for three read-out probes written for this lane. Each
directory is one `lab tv --no-note` export (channel `ws-report-v1`, `rangeProof`
covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8),
`metrics.json`, `meta.json`, on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01.
`tests/test_coof_recalc_security_tapes.cpp` replays them through the Pine adapter
over the bars in `bars.inc`, the 1m bars as the auxiliary request.security feed.

| tape | declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `int28fix-rc` | calc_on_order_fills, fixed 1 | 2 | `ce254341b35a225d58f7b92a3d1c91a7f37bacd33268cc3d5d3c1a980e260025` |
| `int28fix-rc2` | calc_on_order_fills, fixed 2 | 4 | `73eba042e53af31196e53545b879f985a6d220352bf4631976da973d19bfabba` |
| `int28fix-rc3` | calc_on_order_fills, fixed 1 | 2 | `ec1b6445d703947942be9c5024f01629953101b1223f7758cf302649920f93b7` |

Each probe enters long at the 2025-04-11 00:15 UTC bar and short at the 02:15
bar; each entry fills at the next open. A recalculation then closes the
position with a comment naming what it read, so that exit row's `Signal` is the
recalculation's read-out:

- `int28fix-rc`: the open fill's recalculation closes the position. It names the
  chart bar's `close`, `high` and `low`, a 5m `request.security` close and
  RSI(14), and a 60m close, all with `lookahead_off`. The close fills at the
  same open.
- `int28fix-rc2`: the open fill's recalculation places a take-profit for half the
  position one point beyond the fill. The bar's path reaches it (00:30:
  1520.95). The recalculation of that fill closes the rest, naming the same
  values, and that close fills at the bar's next path point (1523.56).
- `int28fix-rc3`: the open fill's recalculation closes the position, naming a
  `lookahead_on` 5m close and the size, first and last element of a 5m
  `request.security_lower_tf` close array.

What they show: a recalculation reads the whole bar, whichever fill it follows.
- Its close, high and low are the chart bar's (1520.73, 1523.56, 1514.04 on the
  00:30 bar).
- Its lookahead_off 5m close is the 5m bar that closes with the chart bar
  (1520.73).
- Its lookahead_on 5m close is the bar's first 5m bar (1521.55).
- Its lower_tf array holds the bar's three 5m closes (1521.55 .. 1520.73).

The engine fed a bar's slice of the auxiliary feed at the bar's close callback,
after its fill recalculations, so a recalculation read the previous chart bar's
finer values (a 5m close of 1519.95).

The replay reads the 5m values only. The RSI reads TradingView's whole 5m
history, and the 60m bar the first cell reads closes before the replayed bars.

The test's fourth probe has no tape. It closes a position immediately at the
close of a bar that fills nothing before it, once mid-run and once on the last
bar. That fill's recalculation runs after the bar's close callback, and must
read the slice that callback fed. Feeding the slice again there would step the
5m site a second time, and on the last bar would find the slice's routing
already cleared.

## Bars

`bars.inc` holds the replayed bars, copied as text from the corpus 15m chart
feed (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`),
2025-04-11 00:00 .. 03:00 UTC, and from the corpus 1m feed it derives from
(sha256 `db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`),
00:00 .. 03:14 UTC; `exec/INT28-FIX-scratch/tools/gen_rc_bars_inc.py` wrote it.
