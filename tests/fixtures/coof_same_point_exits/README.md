# Every exit one path point triggers fills before that point recalculates (R5 lane TAIL-D)

Under `calc_on_order_fills` TradingView fills every exit of one entry that the same path point
triggers before the point recalculates: two exits of one position that share a stop level both fill
at it, even when the recalculation of the first fill calls `strategy.cancel_all()` and
`strategy.close_all()`. The engine recalculated after the first fill, whose cancel_all withdrew the
second exit before the kernel reached it (niravdpatel5588 on NYSE:F 15).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-05), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
`tests/test_coof_same_point_exits_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m1e` | X1 and X2, one unit each at one stop; the recalculation calls cancel_all() and close_all(immediately = true) | 43 | `ab58d634cded2e1198b2412f4aada41a87180ec5b85c8e0fc0f6d91bef4bbefc` | covered |
| `td-m1f` | cancel_all() and close_all() | 43 | `ab58d634cded2e1198b2412f4aada41a87180ec5b85c8e0fc0f6d91bef4bbefc` | covered |
| `td-m1g` | close_all(immediately = true) only (control) | 43 | `ab58d634cded2e1198b2412f4aada41a87180ec5b85c8e0fc0f6d91bef4bbefc` | covered |

## Bars

`bars.inc`: 2025-04-01 00:00 .. 2025-04-04 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
