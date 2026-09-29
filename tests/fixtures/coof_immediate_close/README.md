# A close a mid-leg recalculation places fills at the leg's end, `immediately` or not (R5 lane TAIL-D)

Under `calc_on_order_fills` a fill inside a leg of the bar's path starts a recalculation there.
TradingView fills a close that recalculation places at the END of that leg (the next extreme), at
its print, with `immediately = true` as without it: the -a, -c and -d tapes are one trade list.
The adapter executed an `immediately` close at the recalculating fill's price. On NYSE:F the next
extreme can sit between two cents (H 9.445); the close_all rested its trigger at the booked tick
(9.45), which the path never reaches, where the close of one id already rested at the raw extreme.

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P and NYSE:F 15, 2025-04-01 .. 2025-04-05 (NYSE:F: 2025-04-15)), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
`tests/test_coof_immediate_close_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m1a` | a position of 2 with a take-profit of 1; its fill recalculation calls close_all(immediately = true); process_orders_on_close | 46 | `6704d1ba9d707ed8da21b02e8649b6e04e3ffa9a5f4b37a4fb9c4448a8a4d140` | covered |
| `td-m1b` | the same without process_orders_on_close | 47 | `2130e0e1e23ff523caa3fa5cb6cf0a6777e13218e872b29f9c7c07f3d423d964` | covered |
| `td-m1c` | close_all() (control: the rule the adapter had) | 46 | `6704d1ba9d707ed8da21b02e8649b6e04e3ffa9a5f4b37a4fb9c4448a8a4d140` | covered |
| `td-m1d` | close("P", immediately = true) | 46 | `6704d1ba9d707ed8da21b02e8649b6e04e3ffa9a5f4b37a4fb9c4448a8a4d140` | covered |
| `td-m1h` | -a on NYSE:F 15 (one-cent tick, half-cent extremes), on every bar | 119 | `0ba881f30dd5e66269e1d319c21c6f862c73dae9b772f4240d78e65082e46f4c` | covered |
| `td-m1i` | -d on NYSE:F 15 (control) | 119 | `0ba881f30dd5e66269e1d319c21c6f862c73dae9b772f4240d78e65082e46f4c` | covered |

## Bars

`bars_eth.inc` holds 2025-04-01 00:00 .. 2025-04-04 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`); `bars_ford.inc` holds 2025-04-01 13:30 .. 2025-04-14 19:45 UTC of the f-15 lane
chart feed (sha256 `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`), copied as text by
`exec/TAIL-D-scratch/tools/gen_bars_inc.py`.
