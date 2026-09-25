# Decimal-dust tape (R5 wave H, lane H-MEASURE, row G2-32)

Every day: long lots 0.1, 0.2 and 1.0, one `strategy.order` sell of 0.3,
then `strategy.close_all`. TradingView's decimal quantities close the 0.1 and
0.2 lots whole: three rows a day. In binary64 `0.1 + 0.2 > 0.3`, so the
kernel's FIFO split leaves a 2.8e-17 remnant of the 0.2 lot beside the 1.0 lot;
the Pine source host's 1e-10 sweep (`PineStrategyHost::on_native_applied`)
erases it and books TradingView's rows, a bare host keeps it and closes it as
a fourth row. `tests/test_pine_dust_sweep_paired.cpp`.

| tape (`lab tv`, ws-report-v1, rangeProof covered) | chart | window | trades | tv_trades.csv sha256 | campaign note |
|---|---|---|---|---|---|
| `hm-g232-decimal-dust` | BINANCE:ETHUSDT.P 15 | 2025-06-02 .. 2025-06-06 | 12 | `3a7568a1da42bff6e93eb212a5453116f468e6bb56cae0fe7ae2e3c9929b4e64` | `tv-tape-hm-g232-decimal-dust-3a7568a1` |

`bars.inc` (`exec/H-MEASURE-scratch/report/row3-dust/gen_bars.py`): the corpus
15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived
from the corpus 1m feed `db8c1332…`), 2025-06-02 .. 05, 00:00 .. 02:30 UTC
(eleven bars a day; fixed quantities, so each day replays alone).
