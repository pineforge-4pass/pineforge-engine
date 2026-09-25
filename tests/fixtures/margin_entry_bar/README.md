# Leveraged entry-bar margin tapes (R5 lane H-MEASURE, row M7 / G2-09)

TradingView's own trades for one question AUDIT4 §6 left "ruled, unmeasured":
does TradingView check a LEVERAGED opening (margin below 100 %) on the bar its
entry fills, where the adapter's `margin_check_allowed` refuses the kernel's
AfterApplied point? `tests/test_adapter_margin_schedule_differential.cpp`
(section "M7 on tapes") replays every tape through the Pine adapter and through
a bare kernel host with TradingView's money and slice. TradingView does, on 4
of 4; since R5 lane PAR-MARGIN `on_applied` admits that point for a leveraged
opening's entry bar, and the section asserts the adapter books each tape's
rows (lane H-MEASURE had pinned the adapter's late or missing call instead).

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The sha256 of each
tape is `metrics.json` `tvTradesCsvHash`; the campaign note is
`tv-tape-<slug>-<first 8 hex>`.

| probe | chart | margin, size | entry bar (UTC) | TradingView |
|---|---|---|---|---|
| `hm-m7a-eth-m5-k16-0621` | BINANCE:ETHUSDT.P 15 | 5 %, 16 x equity | 2025-06-21 21:30 | margin call 6.842 @2230 (the entry bar's low), the whole book |
| `hm-m7a-eth-m5-k16-0922` | same | 5 %, 16 x | 2025-09-22 06:00 | margin call 3.793 @4000.88, the whole book |
| `hm-m7a-eth-m5-k16-1121` | same | 5 %, 16 x | 2025-11-21 07:30 | margin call 5.806 @2642.65, the whole book |
| `hm-m7a-eth-m20-k4-1010` | same | 20 %, 4 x | 2025-10-10 21:15 | margin call 0.4544 @3400 (4 x 0.1136), the rest 0.6196 at the timeout @3895 |

All four export windows are 2025-04-01 .. 2026-05-01; each script trades once.
`bars.inc` holds each event's signal bar through the entry bar + 4, from the
corpus feed `corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv` materialized by
`scripts/derive_corpus_feeds.py` at corpus gitlink eede4a2.

## M10: the book a default-percent stop entry above 100 % opens (R5 lane PAR-MARGIN)

Eight more `lab tv` exports on NYSE:F 15m, same channel and layout, margin 25,
one buy stop placed on the session's last bar (15:45 America/New_York) and
filled at the next session's open, closed 3 bars after its entry bar:
`pm-m10-f-k4-*` at 400 % with the stop at 0.9 x close (marketable when
placed), `pm-m10-f-gapup-*` at 390 % with the stop at round(1.002 x close, 2)
(not marketable when placed, gapped through by the open). TradingView opens the
quotient of the signal close on its tick for the first and of the snapped
stop level for the second, never the fill's, and books its margin calls on
that book. `m10_bars.inc` holds each event's bars as TradingView reports them,
read by the two `pm-m10-f-ohlc*` exports (each bar printed as an order
comment); they equal the lab feed `80f404ae85ef`, half cents included.
`tests/test_adapter_margin_schedule_differential.cpp` (section "M10 on tapes")
replays all eight. Campaign notes: `tv-tape-<slug>-<first 8 hex>`.

| probe | window | TradingView | tvTradesCsvHash |
|---|---|---|---|
| `pm-m10-f-k4-0402` | 2026-03-25 .. 2026-04-07 | book 3427; margin calls 176 @11.38 | `3bd3141a7089f30c` |
| `pm-m10-f-k4-0410` | 2025-04-02 .. 2025-04-15 | book 4219; margin calls none | `8911072fc9bfb7ad` |
| `pm-m10-f-k4-1007` | 2025-09-29 .. 2025-10-10 | book 3144; margin calls 980 @12.11 | `bd8d6bc27e858bfb` |
| `pm-m10-f-k4-0309` | 2026-03-02 .. 2026-03-12 | book 3294; margin calls 4 @11.83, 56 @11.81 | `ab895fec3d476066` |
| `pm-m10-f-gapup-0501` | 2025-04-24 .. 2025-05-06 | book 3896; margin calls 376 @10.03 | `c672eb182acd2a16` |
| `pm-m10-f-gapup-0331` | 2026-03-24 .. 2026-04-03 | book 3475; margin calls 156 @11.28 | `8f54192f161b9013` |
| `pm-m10-f-gapup-0424` | 2025-04-17 .. 2025-04-29 | book 3987; margin calls 92 @9.81 | `79a6aa531e7bfbb3` |
| `pm-m10-f-gapup-0527` | 2025-05-19 .. 2025-05-30 | book 3757; margin calls 204 @10.35 | `f6222f524d46cd37` |
| `pm-m10-f-ohlc` | 2025-04-01 .. 2026-04-10 | 24 bars' open, high, low and close printed as entry comments | `8b006a00aff01690` |
| `pm-m10-f-ohlc2` | 2025-04-01 .. 2026-04-10 | 24 bars' open, high, low and close printed as entry comments | `52160db7208aea53` |
