# TradingView's bar-magnifier intrabars (R5 lane MAG-INTRABAR)

Which bars does TradingView's bar magnifier walk inside a chart bar, and which
chart bar owns each? Every directory is one `lab tv` export (`ws-report-v1`,
`rangeProof: covered`) of the lane's own synthetic script, byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json` (the chart, the
window and both sha256 values) and `metrics.json`. `bars_1m.inc` holds the
one-minute bars the replays read: BINANCE:ETHUSDT.P from the corpus feed,
NSE:NIFTY and NYSE:F from the lab's finer feeds (`lab bars <symbol> 1 --feed`).
`tests/test_adapter_magnifier_intrabar_tapes.cpp` reads every directory.

TradingView's help centre ("What is bar magnifier backtesting mode") fixes the
intrabar timeframe per chart timeframe, each row covering the charts up to the
next: 1 -> 10S, 5 -> 30S, 10 -> 1, 15 -> 2, 30 -> 5, 60 -> 10, 240 -> 30,
1D -> 60, 3D -> 240, 1W -> 1D ("15 / 2 yields 7.5 bars, which is rounded down
to 7"). The rule every tape here keeps: an intrabar is the symbol's regular bar
of that timeframe, anchored at the session day's open and cut at its close,
owned by the chart bar that holds its LAST minute; a chart bar's path is its own
open, its owned intrabars, its own close.

## `mi-ltf-*`: TradingView's own intrabars

Each script prints, inside two or three chart bars, TradingView's intrabars of
the table's timeframe through `request.security_lower_tf(syminfo.tickerid,
<intrabar>, ...)` -- their `time`, `time_close` and OHLC -- as entry comments.
The test checks the table, the step, the grid and the ownership, and, where
`bars_1m.inc` carries the chart bars' minutes, that
`source::tradingview_magnifier_bars` builds the same intrabars value for value.

| probe | chart | TradingView prints | tvTradesCsvHash |
|---|---|---|---|
| `mi-ltf-eth-1` | ETH 1 (12:00, 12:01 UTC) | six 10-second bars per chart bar, from its open | `78fc50c2caef4c87` |
| `mi-ltf-eth-5` | ETH 5 (12:00, 12:05) | ten 30-second bars per chart bar | `4579273b682c6242` |
| `mi-ltf-eth-30` | ETH 30 (12:00, 12:30) | six 5-minute bars per chart bar; OHLC = the feed's | `271ba1652be63702` |
| `mi-ltf-eth-60` | ETH 60 (12:00, 13:00) | six 10-minute bars; OHLC = the feed's | `9c3fa5555370e04f` |
| `mi-ltf-eth-240` | ETH 240 (12:00, 16:00) | eight 30-minute bars; OHLC = the feed's | `f2b7fae981d6b69c` |
| `mi-ltf-eth-1D` | ETH 1D | twenty-four 1-hour bars from 00:00 UTC | `7a5383f1d4f5715b` |
| `mi-ltf-nifty-15` | NSE:NIFTY 15 (09:15, 09:30 and 15:15 IST) | 2-minute bars on the 09:15 grid: seven in the 09:15 bar; the 09:29-09:31 one opens the 09:30 bar (eight); 15:15's last one is 15:29-15:30, cut by the close; OHLC = the feed's | `83c03a5ac10384f6` |
| `mi-ltf-f-1D` | NYSE:F 1D (2025-10-07, 10-08) | seven 1-hour bars from 09:30 New York, the last 15:30-16:00; OHLC = the feed's | `53706bc0a768d1fe` |

The 15-minute rows on ETH are `tests/fixtures/intrabar_margin/pm2-ltf-*`
(minutes [14, 16, .., 28] for a :15 bar, [30, .., 42] for a :30 one).

## `mi-fx-*`: magnified bracket tapes

A market long every other chart bar and a +-K tick bracket that often resolves
inside one chart bar (`use_bar_magnifier=true`). Replayed through the adapter on
the one-minute feed with the magnifier on, every exit is TradingView's, bar and
price; walking the feed's own minutes (the adapter before the lane) parts on
5 of 23, 5 of 11 and 4 of 11.

| probe | chart | window | trades replayed | tvTradesCsvHash |
|---|---|---|---|---|
| `mi-fx-eth-15` | ETH 15, K = 400 | 2026-01-29 .. 2026-02-01 | 23 | `260265796c5247fd` |
| `mi-fx-eth-30` | ETH 30, K = 600 | 2026-01-29 .. 2026-02-01 | 11 | `45b52ab30ec2c87b` |
| `mi-fx-nifty-15b` | NSE:NIFTY 15, K = 400 | 2026-01-30 .. 2026-02-03 | 11 | `4ad8cd8cc38baaa8` |

## `mi-coof-refill*`: `calc_on_order_fills` cascades

At a signal bar one market long, then every fill's recalculation adds one while
fewer than six are open, so each lot books the next fill point of the magnified
path. `mi-coof-refill` runs its cascades on the :00 and :15 bars of an even hour
(a :15 bar owns the straddling :14-:16 intrabar), `mi-coof-refill-30` on the
:30 bars. TradingView's fill points: the chart bar's own open (on a straddle
bar, once), then each intrabar's open twice, its nearer extreme and its other
extreme -- never its close; the next intrabar's open follows. The adapter books
every lot of the 72 cascades; walking the feed's own minutes with the chart
bar's waypoint rule, it booked none of them.

| probe | chart | window | cascades | tvTradesCsvHash |
|---|---|---|---|---|
| `mi-coof-refill` | ETH 15 | 2026-01-29 .. 2026-02-03 | 36 | `7bfe24aef5bd47bc` |
| `mi-coof-refill-30` | ETH 15 | 2026-01-29 .. 2026-02-03 | 36 | `3ead34f0e8702e35` |
