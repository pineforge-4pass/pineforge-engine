# Excursion tapes (R5 wave H, lane H-MEASURE, row E19 / G2-22)

TradingView's own favorable / adverse excursion per trade, against the Pine
adapter's host-owned model (RULING A48) and the kernel sampler.
`tests/test_e19_excursion_tape.cpp` replays the rows below through a
`PineStrategyHost` and, with the same requests, a bare `NativeStrategyHost`.

| tape (`lab tv`, ws-report-v1, rangeProof covered) | chart | window | trades | tv_trades.csv sha256 | campaign note |
|---|---|---|---|---|---|
| `hm-e19-stop-reversal-grouping` (corpus probe `order-stop-entry-reversal-grouping-01`'s own `strategy.pine`) | BINANCE:ETHUSDT.P 15 | 2025-04-01 .. 2026-05-01 | 1580 | `1748651d9064e7844e67ed1075a0bc43488600720878cd4a46c61be8fce44a5d` | `tv-tape-hm-e19-stop-reversal-grouping-1748651d` |
| `hm-e19-coof-scratch` | BINANCE:ETHUSDT.P 15 | 2025-06-02 .. 2025-06-06 | 4 | `fe94432eb27eb032cb318c0e74ff6b6f95a925bfff2a56bd7217f48cca306244` | `tv-tape-hm-e19-coof-scratch-fe94432e` |

The first tape's excursion cells equal the corpus tape's
(`corpus/validation/order-stop-entry-reversal-grouping-01/tv_trades.csv`) on
all 1460 trades the two share. Only its twelve rows on three days are carried
here (`tv_rows.inc`, cells verbatim, generated with the tape's sha256); the
full CSV (3160 rows) is the campaign note's. The second tape is carried whole.

`bars.inc` (`exec/H-MEASURE-scratch/report/row1-excursion/gen_fixture.py`):
the corpus 15m chart feed that `scripts/derive_corpus_feeds.py` derives from
the corpus 1m feed at gitlink eede4a2 (1m sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`, 15m sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`):
2025-04-08, 2025-05-13 and 2025-10-17 00:00 .. 02:15 UTC (ten bars each) and
2025-06-02 .. 05 00:00 .. 00:45 UTC (four bars each).
