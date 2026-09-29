# A close a mid-leg recalculation places fills at the leg's end, `immediately` or not (R5 lane TAIL-D)

Under `calc_on_order_fills` a fill inside a leg of the bar's path starts a recalculation there.
TradingView fills a close that recalculation places at the END of that leg (the next extreme), at
its print, with `immediately = true` as without it: the -a, -c and -d tapes are one trade list.
The adapter executed an `immediately` close at the recalculating fill's price. On NYSE:F the next
extreme can sit between two cents (H 9.445); the close_all rested its trigger at the booked tick
(9.45), which the path never reaches, where the close of one id already rested at the raw extreme.

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P and NYSE:F 15, 2025-04-01 .. 2025-04-05 (NYSE:F: 2025-04-15)), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
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

## An immediate close in the bar's calculation (R5 lane TAIL-E)

TradingView's trades of one synthetic probe with and without
`calc_on_order_fills` (R5 lane TAIL-E). Each directory is one
`lab tv --no-note` export (channel `ws-report-v1`), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8, the exporter's rendering),
`metrics.json` and `meta.json`. The probes are synthetic and public; no
scraped or closed strategy is involved.

`te_coof_immediate_reentry` (strategy.pine sha256
`360b0611590292c16c254f3f538a906489f9a8147c8b4b39c743bb1442f75d8d`) trades one
contract, pyramiding 1, with `calc_on_order_fills = true`. At the close of
every eighth bar (`bar_index % 8 == 5`) it closes its position with
`strategy.close_all(immediately = true)` and sends a market entry `A` after
it; four bars later (`== 1`) it sends an entry `B` first and the immediate
close `Y` after it; on `== 3` it enters `C` when flat. Every order names the
bar it was sent on. `te_plain_immediate_reentry`
(`d9ed023f698a8c5c6e11c44d92e102a7c27c561cac9fead67f719de144925633`) is the
same script without `calc_on_order_fills`.

| tape | chart | window | rows | tv_trades.csv sha256 | exported (UTC) | rangeProof |
|---|---|---|---|---|---|---|
| `te-coof-immediate-reentry-eth15` | BINANCE:ETHUSDT.P 15 | 2025-04-01..04-03 | 96 | `f6fddd49d09b9089676c1b159d5353a8e0ce38b6d9f3f2ee35ef363db68c868e` | 2026-09-29 00:40:58 | covered |
| `te-plain-immediate-reentry-eth15` | BINANCE:ETHUSDT.P 15 | 2025-04-01..04-03 | 96 | `f6fddd49d09b9089676c1b159d5353a8e0ce38b6d9f3f2ee35ef363db68c868e` | 2026-09-29 01:04:30 | covered |

`bars.inc` holds the chart's bars for the replay: the corpus 15-minute feed
`scripts/derive_corpus_feeds.py` derives from `corpus/data/ohlcv_ETH-USDT-USDT_1m.csv`,
rows 2025-04-01 00:00 .. 04-03 00:15 UTC, copied as text (the two feeds'
sha256 are in its header).

### What they show

The two tapes are the same bytes: no fill recalculation follows a close the
bar's own calculation executes. The close `X5` fills at bar 5's close and the
entry `A5` sent behind it at bar 6's open, once. `B9`, sent while the position
held the pyramiding limit, never fills, though `Y9` flattens the position on
the same bar; the next entry is `C11`, at bar 12's open. The trade TradingView
closes at the range's end has no exit signal; the replay leaves it out.

`tests/test_coof_immediate_close_final_tapes.cpp` replays both tapes through
the Pine adapter and requires every trade they close -- entry and exit time,
price in ticks, quantity and the orders' names -- to be the engine's.
