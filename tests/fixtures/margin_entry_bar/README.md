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
