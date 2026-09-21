# Trail activation tick-reach tapes (R5 follow-up lane E9)

TradingView's own trades for two questions lane E5 left open (its report,
"(f) Findings"): which price a ONE-SHOT trail (`trail_offset` 0) books when
its `trail_price` is sub-tick, and whether a PLACEMENT close that sits inside
the activation's tick cell counts as already reached.
`tests/test_trail_activation_tick_reach.cpp` replays every trade below through
the Pine adapter and the kernel.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The sha256 of each
tape is `metrics.json` `tvTradesCsvHash`. `e5-eth-long-oneshot-p004` was
exported by lane E5 (its one-shot control, left out of
`tests/fixtures/offset_trail_arm` because the engine failed it); the others by
this lane.

| probe | chart | window | exit | trades | TradingView |
|---|---|---|---|---|---|
| `e5-eth-long-oneshot-p004` | BINANCE:ETHUSDT.P 15 | 2025-04-20 .. 2026-04-25 | `trail_price` = entry fill + 0.004, `trail_offset` 0 | 8 | 5 exit on the first bar whose path reaches the level, at the tick PAST it (fill 2550.85, level 2550.854: @2550.86); 3 never reach it and close at the timeout |
| `e9-eth-short-oneshot-m004` | same | same | entry fill - 0.004, `trail_offset` 0 | 8 | the short twin: 4 exit at the tick past the level (fill 1791.83: @1791.82), 4 at the timeout |
| `e9-f-long-cell-off1` | NYSE:F 15 | 2025-04-10 .. 2026-04-17 | `trail_points` n, `trail_offset` 1 | 7 | the placement close (11.295, 11.575, 13.055, 14.135, 13.705, 13.385, 11.565) is half a tick short of the activation and quantizes onto it (11.30, 11.58, 13.06, 14.14, 13.71, 13.39, 11.57); every trade exits on the NEXT bar, at its open = activation - 1 tick |
| `e9-f-long-cell-oneshot` | same | same | same entries, `trail_offset` 0 | 7 | identical tape: the next bar's open |
| `e9-f-short-cell-off1` | same | same | `trail_points` 2, `trail_offset` 1 | 1 | placement close 14.415 above the 14.41 activation, quantized onto it: exits on the next bar's open 14.42 = activation + 1 tick |
| `e9-f-short-cell-oneshot` | same | same | same entry, `trail_offset` 0 | 1 | identical tape |
| `e14-f-long-shallow-next` | NYSE:F 15 | 2025-04-01 .. 2026-04-10 | `trail_points` n, `trail_offset` **5** | 7 | R5 lane E14: the placement close reaches the activation on its tick here too, but the NEXT bar is SHALLOW — it opens half a tick to a tick short of the activation and never trades back to it, while its low reaches the carried stop exactly. Every trade exits on that next bar at the carried best - 5 ticks; a best that restarted at the bar's own first print would exit 4 to 18 bars later |
| `e14-f-short-shallow-next` | same | same | `trail_points` n, `trail_offset` **5** | 4 | the short twin, and the one that pins the carried best RAW: each placement close sits half a tick PAST the activation (13.035 under 13.04, 13.925, 14.075, 13.295), TradingView's stop is that close + 5 ticks, and the next bar's high reaches it where the activation's own stop never would. The booked price is the grid point at or above that stop (13.085 -> 13.09): a trail stop is reached from the adverse side, the other direction from the one-shot limit above |

The one-shot rests at the half-tick boundary where the tick-quantized path
first reaches its level (2550.855 for 2550.854), so the price the quantized
path reaches there, and the one TradingView books, is that level rounded AWAY
from the position. The placement close is one more print of that quantized
path: a close whose tick reaches the activation has reached it, and the trail's
running best starts at the activation (the `trail_offset` 1 exits are
activation -/+ 1 tick, not close -/+ 1 tick). On every NYSE:F event the next
bar never reaches the activation's half-tick boundary on its raw path, so a
trail read raw at placement stays dormant on it; TradingView exits there 8 of
8.

Lane E14's two tapes answer the residual lanes E5 and E9 both recorded: WHERE
the running best starts. The eight tapes above all had a next bar that fell
far enough for either reading of it to exit; these eleven trades were chosen
so the next bar's own extreme lies BETWEEN the two stops. TradingView exits
all eleven on that next bar, so its running best is the carried level — the
activation, or the placement print when that print is already past it — and
never the leg's first live print. One trade of the long tape is a recorded
divergence the engine still fails and this question does not reach:
2025-09-10's stop is 11.44 - 5 ticks, whose binary64 value
11.389999999999998792 lies one ULP under the bar's low 11.390000000000000568,
so the raw trail stop (adapter policy by lane E5's ruling) is not reached.
That is the tick-product residual on the trail stop, and it needs its own
lane.

`strategy.exit` with `trail_points` and no `trail_offset` is refused by
TradingView's compiler ("strategy.exit must have at least one of the following
parameters: "profit", "limit", "loss", "stop" or one of the following
pairs: ..."), so the omitted-offset shape has no tape of its own.

Bars:

- ETH: `tests/fixtures/offset_trail_arm/bars.inc` (`kEthLong`, `kEthShort`),
  the entries lane E5 exported its ETH tapes over, from
  `corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv` at corpus gitlink 442d497,
  sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`.
- NYSE:F: `bars.inc` here (`kFordLongCell`, `kFordShortCell`, and lane E14's
  `kFordLongShallow`, `kFordShortShallow`), per entry the
  feed bars from the placement bar through the bar the probe's timeout close
  would fill on, from the registry feed `lab bars NYSE:F 15`, evidence sha256
  `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`
  (`lab evidence get <sha> --out <file>`).
