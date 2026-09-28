# A close_all placed after an all-in reversal entry (lane INT28-FIX, rule CA)

At Pine v6's defaults (100% of equity per order, margin 100 both ways) a
default-size reversal entry is declined at the next open when its units at
that open cost more than the equity they were sized from. A
`strategy.close_all()` placed on the same bar after that reversal entry
belongs to it, as a `strategy.close()` of the held id does: TradingView
flattens the held position through neither. Where the open funds the
reversal, the reversal closes the position and opens the other side, and the
close_all does not flatten that side; where the open declines it, the
position is held and the close_all does not fill either. The lowering had
placed the close_all as a `Flatten` request of its own, which flattened the
held position at a declined open (population probe
`job-2614-andrewwieiw-frosty-alerts` on BINANCE:ETHUSDT.P 15, whose
end-of-window `strategy.close_all()` follows its reversal entries every day).

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-04-17, and none declares `initial_capital`, so each runs
on v6's 100000. All five run the same twelve `timestamp("UTC", ...)` cells: a
position opened at a cell's close (long at the even cells, short at the odd
ones), the cell's orders at the close 30 minutes later, and a cleanup
(`strategy.cancel_all()`, `strategy.close_all()`) 90 minutes after the cell.
At 100% the next open declines the reversal of eight cells (04-01 01:30,
04-02 12:30, 04-03 06:15 and 18:15, 04-04 15:30, 04-06 19:45, 04-07 03:45
and 08:45 UTC) and funds the other four.

| tape | trades | the cell's orders | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `int28fix-ca2-rev` | 25 | the reversal entry, then `strategy.close_all()` | holds the eight declined positions to the cleanup and reverses the four funded ones; no close_all fills | `5a5deba25e2f644dd70dc8b70a214a7ccd08f61b39b9852ef1a1e274482eea20` |
| `int28fix-ca2-frosty` | 25 | job-2614's order: the long entry, the short entry, `strategy.close()` of the held id, `strategy.close_all()` | the same file | `5a5deba25e2f644dd70dc8b70a214a7ccd08f61b39b9852ef1a1e274482eea20` |
| `int28fix-ca2-close-id` | 25 | the control: the reversal entry, then `strategy.close()` of the held id | the same file | `5a5deba25e2f644dd70dc8b70a214a7ccd08f61b39b9852ef1a1e274482eea20` |
| `int28fix-ca2-first` | 40 | the control: `strategy.close_all()`, then the reversal entry | flattens every cell at the open ("flatten") and opens the other side from flat there | `7cce6113b99a1b25498b5970c778d9aa4b18abae1bcd253d943a8c7df030185d` |
| `int28fix-ca2-p99` | 30 | the control: `int28fix-ca2-rev` at `default_qty_value = 99` | no reversal is declined: every cell reverses, and no close_all fills | `e46f1e543f67f2a29a0c70ba149bfe051bacb30ae099f73283aea50c0a10ce82` |

`tests/test_reversal_close_all_tapes.cpp` replays every tape through the Pine
adapter under the configuration its `strategy()` declares, over the corpus
15m bars in `tests/fixtures/margin_v6/bars.inc`, with TradingView's lot
(0.0001 ETH) as the `qty_step`, and requires each trade the tape closes --
entry and exit time, side, price in ticks of 0.01, quantity in lots -- to be
the engine's, and at 100% the engine to close through the cells' close_all
exactly where the tape does (at 99% the rule does not apply: the engine
flattens at the open and opens the other side there, which books the same
trades). On the lane's base (4efcb8c5) the two rule tapes fail there and the
three controls pass.
