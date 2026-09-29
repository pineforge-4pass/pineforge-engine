# An all-in commissioned and slipped short's margin calls (R5 lane TAIL-I)

TradingView's own trades for one synthetic script, drafted by lane TAIL-C's diagnosis and
exported by lane TAIL-I: every 8 bars from flat one explicit-quantity MARKET short of
`math.floor(strategy.equity / close / lot) * lot` is placed, filled at the next open less
2 ticks, and flattened by `strategy.close_all()` 4 bars later. 10000 of capital, margin
100 both ways, slippage 2, commission 0.05 %, pyramiding 1. Such a short opens short of
margin as a rule -- its fill is slipped and its entry fee paid -- so nearly every cell is
margin-called on its entry bar.

What they show:

- **O, the opening print.** The entry bar is checked from its open: the book, its entry fee
  paid, marked at the open's tick is called four times its lot-floored shortfall there,
  filled at that tick plus the slippage, before the rest of the bar's path. On NYSE:F
  2025-04-01 15:30 UTC a short of 1006 shares filled at 9.92 off the 9.935 open is
  24.75 short at the 9.94 tick: 8 shares at 9.96. The engine deferred a commissioned
  explicit-quantity short to a post-script trim at its fill and one slice at the bar's
  high (32 at 9.99 there).
- **P, every point of the path.** Each later point -- the extreme nearer the open, the
  other, the close -- is checked on the book the calls before it left, the points after
  the bar's adverse extreme too: where the extreme's one-unit call would not restore the
  book at its own fill, the next point's can (`-f3` 2025-09-22 18:00 UTC: none at the
  11.63 high, one share at the 11.615 low).
- **F, the follow-up.** Under a percent commission a call of any size is followed: while
  the book it called, re-marked at its fill, is short by more than the called units'
  margin and two slippage steps each (one more unit's exit fee added), the next path
  point inside that fill takes four times the shortfall's lot floor over the fill, or
  one unit where that floors to none, and a follow-up is followed alike (`-f`
  2025-04-02 15:00 UTC: 4 at the open, 4 at the high, 1 at the low, then 4 and 4 on the
  16:00 bar; `-f3` 2025-07-31 16:45 UTC: 0.185 short after 4 called, none). On OANDA:EURUSD's
  0.01 lots as on NYSE:F's shares (`-eur` 2025-04-21 08:45 UTC: 0.16 at the high, 0.44 at
  the close).

`tests/test_margin_open_print_tapes.cpp` replays each tape through the Pine adapter over
the lane chart feed of its window, from the first bar TradingView computed, with
TradingView's lot as the `qty_step`, and requires every trade the tape closes before the
row's end to be the engine's: entry and exit time, side, price in ticks, quantity in lots.

| tape | chart, window | trades | compared (margin calls) | tv_trades.csv sha256 |
|---|---|---:|---:|---|
| `taili-mop-f` | NYSE:F 15, 2025-04-01 .. 2025-07-01 | 445 | 36 (25), to 2025-04-07 15:30 UTC | `5eca5b6d53f9367c04258dddd4aba34aa9150208f5b8f6ba0fb92d20223aadb4` |
| `taili-mop-f3` | NYSE:F 15, 2025-07-01 .. 2025-10-01 | 446 | 445 (278), all closed | `06dbe45f427ebf92d045f684c5398bb5ca37e7ec64d2f64dd36277dd6d90d1d5` |
| `taili-mop-eur` | OANDA:EURUSD 15, 2025-04-01 .. 2025-05-01 | 701 | 625 (408), to 2025-04-28 14:00 UTC | `d422ebf370faf70f6752c90c200af718674024bcee75cd15a3cfb0c6b84bafa7` |

Bars: `../slipped_short/bars.inc` (NYSE:F 2025-04-01 .. 2025-06-30), `f15_q3_bars.inc`
(NYSE:F 2025-07-01 .. 2025-09-30; both the f-15 lane chart feed, evidence sha256
`80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`) and
`../margin_residual/eur15_bars.inc` (OANDA:EURUSD 2025-04-01 .. 2025-04-29, the rows the
eurusd-15 lane chart feed `c4ffa3e17348d80aec5678a299ebf6708525c83b7cb636dba640cdb47cffd03f`
holds unchanged).

Open, where the windows end:

- `-f` 2025-04-07 15:30 UTC: a short of 1024 shares filled at 9.09 off the 9.105 open --
  TradingView's own bar, read out by a lane tape -- is 16.64 short at the 9.11 tick, and
  TradingView calls nothing there; its one call is 180 at the bar's 9.33 high, the call
  the high owes the whole book. A second NYSE:F export from 2025-04-08 shows the same at
  its first cell (2025-04-08 17:30 UTC). Of the 209 cells of the three NYSE:F exports whose
  book is a unit or more short at the open's tick, TradingView calls the other 207 there.
- `-eur` 2025-04-28 14:00 UTC: the 14:15 high leaves the book 0.043 short, a call of 0.12;
  TradingView takes none, a difference of the equity's last cents.

Each directory is one `lab tv --no-note` export (maintainers' private pineforge-workflow, channel
`ws-report-v1`, `rangeProof` covered) of the synthetic script, byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`, `meta.json`.
