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
  windows. Explained by R5 lane PAR-MARGIN-2 (below): TradingView's magnifier
  walks 2-minute intrabars on a 15-minute chart, and 01:42-01:43 is ONE of them,
  its low 2673.33. Recorded: the adapter samples the one-minute feed.

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

## PAR-MARGIN-2: item 3 under `process_orders_on_close`, `calc_on_order_fills` and on a full-margin book

Twenty more `lab tv` exports on BINANCE:ETHUSDT.P 15m, the lane's own scripts:

- `pm2-i3-coof-*`: this directory's four magnified scripts under
  `calc_on_order_fills` -- the same rows as `pm-i3-eth-mag-*` (the same tape
  hashes), 0130's second call included.
- `pm2-i3-pooc-{mag,chart}-*`: a leveraged long (margin 20) filled at the
  signal close under `process_orders_on_close`, carried into a bar that first
  crosses at an intrabar low and reaches a deeper low later; magnifier on and
  off.
- `pm2-i3-s1x-{mag,chart}-*`: a 1 x short (margin 100, 0.93-1.00 x equity)
  filled at the next open, carried into a bar that first crosses at an
  intrabar high and reaches a higher one later; magnifier on and off.

Magnifier off: one call at the chart bar's extreme, as the adapter books it (8
of 8). Magnifier on: TradingView checks the book at each of ITS intrabars --
2 minutes on a 15-minute chart (TradingView's bar magnifier table: 15 -> 2;
"15 / 2 yields 7.5 bars, which is rounded down to 7") -- and books the call at
the first one whose extreme crosses, sized there, and again at each later one
that crosses the reduced book's line. The `pm2-ltf-*` probes print TradingView's
own 2-minute intrabars inside six chart bars through
`request.security_lower_tf(syminfo.tickerid, "2", ...)`: epoch-aligned pairs of
the feed's minutes, each owned by the chart bar that holds its LAST minute
([14, 16, .., 28] for a :15 bar, [30, .., 42] for a :30 one), so a :15 bar's
first intrabar reaches back into the previous bar's last minute (0402's first
call, 1908.17, is that minute's low: it precedes the fill at the signal close).

`tests/test_adapter_margin_schedule_differential.cpp` ("intrabar shapes")
replays every tape from its own `strategy.pine` and models the check: at
TradingView's 2-minute intrabars the model books every call of all 16
magnified tapes here (and of the four `pm2-m7-mag-*`), and at the one-minute
samples the adapter's host feeds it books exactly what the adapter books. The
two grids agree on 10 of the 16, where the adapter books the tape's rows; they
part on six (0130 plain and COOF, pooc 0109, 0402 and 0707, s1x 0623), which
stay recorded. `pm2-ltf-*` is checked against the feed ("TradingView
intrabars"). `pm2_bars_1m.inc` holds each new event's one-minute bars from the
corpus feed.

| probe | window | TradingView's margin calls | tvTradesCsvHash |
|---|---|---|---|
| `pm2-i3-coof-0130` | 2026-01-28 .. 2026-02-01 | 0.1088 @2760, 0.4824 @2673.33 | `ac0fb36013b01d07` |
| `pm2-i3-coof-0406` | 2025-04-04 .. 2025-04-08 | 0.418 @1574.55 | `40208b4aa509bd58` |
| `pm2-i3-coof-0622` | 2025-06-20 .. 2025-06-24 | 0.1052 @2223.05, 0.0276 @2198.19, 0.1688 @2180 | `2488da65fe9bd341` |
| `pm2-i3-coof-0824` | 2025-08-22 .. 2025-08-26 | 0.1224 @4820 | `f0e1b58462313b44` |
| `pm2-i3-pooc-chart-0109-0115` | 2026-01-07 .. 2026-01-11 | 0.2864 @3077.46 | `4dea9978d6d5e886` |
| `pm2-i3-pooc-chart-0402-2000` | 2025-03-31 .. 2025-04-04 | 0.8216 @1872.48 | `c592ff0981fbb444` |
| `pm2-i3-pooc-chart-0707-1600` | 2025-07-05 .. 2025-07-09 | 0.2712 @2505.37 | `7093ef952abe4bdc` |
| `pm2-i3-pooc-chart-1010-1900` | 2025-10-08 .. 2025-10-12 | 0.2436 @3950 | `34e625565aa35031` |
| `pm2-i3-pooc-mag-0109-0115` | 2026-01-07 .. 2026-01-11 | 0.1332 @3096.44 | `a0d512ec52485a02` |
| `pm2-i3-pooc-mag-0402-2000` | 2025-03-31 .. 2025-04-04 | 0.0656 @1908.17, 0.1992 @1888.67 | `70d257b985aec02f` |
| `pm2-i3-pooc-mag-0707-1600` | 2025-07-05 .. 2025-07-09 | 0.2712 @2505.37 | `7093ef952abe4bdc` |
| `pm2-i3-pooc-mag-1010-1900` | 2025-10-08 .. 2025-10-12 | 0.0392 @3991.74, 0.0788 @3950 | `c9aa3967619f2f8a` |
| `pm2-i3-s1x-chart-0201-1100` | 2026-01-30 .. 2026-02-03 | 0.0352 @2423.66 | `50f0f64a37926674` |
| `pm2-i3-s1x-chart-0407-1145` | 2025-04-05 .. 2025-04-09 | 0.0872 @1519.12 | `7f3c307947114fba` |
| `pm2-i3-s1x-chart-0623-2130` | 2025-06-21 .. 2025-06-25 | 0.0796 @2409.52 | `8f7d4ee9bdb9ff4b` |
| `pm2-i3-s1x-chart-1104-2130` | 2025-11-02 .. 2025-11-06 | 0.0292 @3271.1 | `098b32d635fd010d` |
| `pm2-i3-s1x-mag-0201-1100` | 2026-01-30 .. 2026-02-03 | 0.0092 @2423.66, 0.0064 @2402.65 | `a72c25a00038c741` |
| `pm2-i3-s1x-mag-0407-1145` | 2025-04-05 .. 2025-04-09 | 0.0084 @1516, 0.0168 @1498.93, 0.0016 @1523.99, 0.0036 @1526.48 | `4a145425eba491a3` |
| `pm2-i3-s1x-mag-0623-2130` | 2025-06-21 .. 2025-06-25 | 0.0236 @2369.42, 0.0236 @2440 | `924d02a624c08979` |
| `pm2-i3-s1x-mag-1104-2130` | 2025-11-02 .. 2025-11-06 | 0.0064 @3271.1, 0.0056 @3238.56 | `5bcd717fe804192c` |

| probe | the chart bar (UTC) | intrabars printed | tvTradesCsvHash |
|---|---|---|---|
| `pm2-ltf-0109` | 2026-01-09 01:30 | minutes [30, 32, 34, 36, 38, 40, 42] | `88eaca621366d3aa` |
| `pm2-ltf-0130` | 2026-01-30 01:30 | minutes [30, 32, 34, 36, 38, 40, 42] | `75e6678ceb46c597` |
| `pm2-ltf-0402` | 2025-04-02 20:15 | minutes [14, 16, 18, 20, 22, 24, 26, 28] | `24f3cae40c441e68` |
| `pm2-ltf-0623` | 2025-06-23 22:00 | minutes [0, 2, 4, 6, 8, 10, 12] | `3223f6f0c7a3462d` |
| `pm2-ltf-0707` | 2025-07-07 16:15 | minutes [14, 16, 18, 20, 22, 24, 26, 28] | `53d673f2e8c5317b` |
| `pm2-ltf-1010` | 2025-10-10 19:15 | minutes [14, 16, 18, 20, 22, 24, 26, 28] | `01b82dc91bf8bbed` |
