# timeframe.change() on a bar a fill recalculated (R5 lane TAIL-D)

`timeframe.change("D")` compares the script bar with the bar before it, on every calculation of the
bar. A fill's recalculation published its bar early and moved the host's previous-bar clock onto the
bar, so the bar's close calculation saw no day change. TradingView resets a day's counter on the
close of a day's first bar that a fill recalculated (niravdpatel5588 on NYSE:F 15: its daily trade
counter never reset on days whose first bar filled a stop).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-15), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
`tests/test_coof_timeframe_change_tapes.cpp` replays the tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m3a` | A at 23:45 with a stop 1 under its close (it fills on the next day's first bar); a counter reset by timeframe.change("D") gates B at 12:00 | 28 | `d32700ab2e9d4e4dc16d9b56637580519ba810e4c69b366fab30c4d87766ce46` | covered |

## Bars

`bars.inc`: 2025-04-01 00:00 .. 2025-04-14 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
