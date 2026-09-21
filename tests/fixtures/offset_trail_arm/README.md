# Offset-trail arm tapes (R5 follow-up lane E5)

TradingView's own trades for the question lane P9 left open: does a
`strategy.exit` trail WITH a trailing offset arm (activate) on the raw price
path or on the tick-quantized one? `tests/test_offset_trail_quantized_arm.cpp`
replays every trade below through the Pine adapter and the kernel.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The sha256 of each
tape is `metrics.json` `tvTradesCsvHash`.

| probe | chart | window | trail | trades | TradingView |
|---|---|---|---|---|---|
| `e5-f-short-arm-qlow` | NYSE:F 15 | 2025-04-10 .. 2026-04-17 | `trail_points` 8 / 5 / 2, `trail_offset` 1 | 3 | exits on the bar whose low 9.415 / 13.041 / 12.641 QUANTIZES onto the activation 9.41 / 13.04 / 12.64, @9.42 / 13.05 / 12.65 |
| `e5-f-long-arm-qhigh` | NYSE:F 15 | same | `trail_points` 25 / 2 / 5, `trail_offset` 1 | 3 | exits on the bar whose high 11.899 / 13.049 / 13.419 quantizes onto 11.90 / 13.05 / 13.42, @11.89 / 13.04 / 13.41 |
| `e5-f-short-oneshot-qlow` | NYSE:F 15 | same | same entries, `trail_offset` 0 | 3 | control: the one-shot exits on the same bars at the activation |
| `e5-f-long-oneshot-qhigh` | NYSE:F 15 | same | same entries, `trail_offset` 0 | 3 | control, same |
| `e5-eth-long-arm-p004` | BINANCE:ETHUSDT.P 15 | 2025-04-20 .. 2026-04-25 | `trail_price` = entry fill + 0.004, `trail_offset` 1 | 8 | a later bar's high equals the fill exactly: never armed there; armed on the first print a tick past the fill |
| `e5-eth-long-arm-p006` | same | same | entry fill + 0.006 | 8 | identical tape |
| `e5-eth-long-points-04` | same | same | `trail_points` 0.4, `trail_offset` 1 | 8 | identical tape |
| `e5-eth-short-arm-m004` | same | same | entry fill - 0.004, `trail_offset` 1 | 8 | a later bar's low equals the fill exactly: never armed there |
| `e5-eth-short-arm-m006` | same | same | entry fill - 0.006 | 8 | identical tape |

On NYSE:F (mintick 0.01, sub-cent prints) the raw path reaches none of the
six activations on the bar TradingView exits on, so the arm is tested on the
tick-quantized path, like the one-shot's, and the booked exit is
activation -/+ 1 tick: the running best starts at the activation. On the
on-grid ETH feed the quantized path IS the raw path; those tapes pin that the
sub-tick LEVEL is not rounded to the tick (half-up would arm on the bar that
touches the fill, 0 of 32 did).

`bars.inc` holds, per entry, the feed bars from the placement bar through the
bar the probe's timeout close would fill on:

- NYSE:F: the registry feed `lab bars NYSE:F 15`, evidence sha256
  `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`
  (`lab evidence get <sha> --out <file>`).
- ETH: `corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv` materialized by
  `scripts/derive_corpus_feeds.py` from the corpus at gitlink 442d497, sha256
  `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`.
