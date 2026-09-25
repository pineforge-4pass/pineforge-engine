# Intrabar margin tapes (R5 lane PAR-MARGIN, item 3)

Does TradingView check the margin model at each intrabar sample under the bar
magnifier? Each pair is one `lab tv` export on BINANCE:ETHUSDT.P 15m
(`ws-report-v1`, `rangeProof: covered`), byte-identical, of the same script with
`use_bar_magnifier` on (`pm-i3-eth-mag-*`) and off (`pm-i3-eth-chart-*`): one
leveraged long (margin 20, 4.6-4.7 x equity) placed on a signal bar, filled at
the next bar's open and carried into a bar that first crosses the maintenance
line at an early 1-minute low and reaches a deeper low later in the same 15m
bar; closed 3 bars after its entry bar. Times in `tv_trades.csv` are UTC+8;
TradingView dates a magnified fill at its chart bar's open.

- Magnifier off: one margin call, at the chart bar's low (4 of 4).
- Magnifier on: the first call at the first 1-minute low that crosses, sized
  there (4 of 4), and a further call at each later 1-minute low that crosses the
  reduced book's line -- every row on 0622, 0824 and 0406. On 0130 TradingView
  books its second call at the 01:43 low (2673.33) on the book the first call
  left, where a check at the 01:42 low (2690, which crosses that book's line)
  would have booked one a minute earlier; the export repeats over three other
  windows. Recorded, open.

`bars_1m.inc` holds each event's 1-minute bars from the corpus feed;
`pm-i3-eth-ltf-lows` prints TradingView's own 1-minute lows inside the two
multi-call bars through `request.security_lower_tf`, and they equal the feed's.
`tests/test_adapter_margin_schedule_differential.cpp` (section "intrabar margin
on tapes") replays every pair. Campaign notes: `tv-tape-<slug>-<first 8 hex>`.

| probe | window | TradingView | tvTradesCsvHash |
|---|---|---|---|
| `pm-i3-eth-mag-0622` | 2025-06-20 .. 2025-06-24 | margin calls 0.1052 @2223.05, 0.0276 @2198.19, 0.1688 @2180 | `2488da65fe9bd341` |
| `pm-i3-eth-chart-0622` | 2025-06-20 .. 2025-06-24 | margin calls 1.064 @2159.28 | `714f4333cebd50c8` |
| `pm-i3-eth-mag-0130` | 2026-01-28 .. 2026-02-01 | margin calls 0.1088 @2760, 0.4824 @2673.33 | `ac0fb36013b01d07` |
| `pm-i3-eth-chart-0130` | 2026-01-28 .. 2026-02-01 | margin calls 0.988 @2673.33 | `2f0f39543f3d42d9` |
| `pm-i3-eth-mag-0824` | 2025-08-22 .. 2025-08-26 | margin calls 0.1224 @4820 | `f0e1b58462313b44` |
| `pm-i3-eth-chart-0824` | 2025-08-22 .. 2025-08-26 | margin calls 0.3784 @4740.72 | `982df67044dacfa5` |
| `pm-i3-eth-mag-0406` | 2025-04-04 .. 2025-04-08 | margin calls 0.418 @1574.55 | `40208b4aa509bd58` |
| `pm-i3-eth-chart-0406` | 2025-04-04 .. 2025-04-08 | margin calls 1.0776 @1552.5 | `ea61c13201510514` |
| `pm-i3-eth-ltf-lows` | 2025-06-20 .. 2026-02-01 | the 1m lows inside 2025-06-22 13:15 and 2026-01-30 01:30 (UTC), printed as entry comments | `ad312f3e1202becf` |
