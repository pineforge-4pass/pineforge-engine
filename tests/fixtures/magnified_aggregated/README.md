# Magnified aggregated tapes (R5 lane H-MEASURE, AUDIT4 X14 "F1's magnified residual")

Lane F1's differential probe (its eight variants v0..v7) as
Pine, on NYSE:F 15m over the three days `tests/fixtures/session_islastbar/bars.inc`
holds (2025-07-02, the 2025-07-03 half day, 2025-07-07); the script's bar counter
starts at 2025-07-02 09:30 ET, the fixture's first bar.
`tests/test_magnified_aggregated_tape.cpp` replays the magnifier-on tapes on the
1m bars aggregated to 15m under the magnifier, and the magnifier-off tapes on the
15m chart and the plain aggregation.

Each directory is one `lab tv` export (`ws-report-v1`, `rangeProof: covered`,
window 2025-07-01 .. 2025-07-08), byte-identical.

| probe | variant | trades | tvTradesSha256 | note |
|---|---|---|---|---|
| `hm-mag-diff-v0` | plain, magnifier on | 12 | `53c28aaf8caa6ed1708a9aa42f2965db37724c87d95691fb16bbb56dbb69906e` | `tv-tape-hm-mag-diff-v0-53c28aaf` |
| `hm-mag-diff-v1` | process_orders_on_close, magnifier on | 12 | `929f16389c8bcd20b60776757c0ceb2482b61ef82a53314ca2218649b14c7069` | `tv-tape-hm-mag-diff-v1-929f1638` |
| `hm-mag-diff-v2` | calc_on_order_fills, magnifier on | 12 | `444e7bc4eaac5b16e224007225bc55ed1c36fecd4453b8eab95e6c1ea70c6656` | `tv-tape-hm-mag-diff-v2-444e7bc4` |
| `hm-mag-diff-v4` | slippage 2, pyramiding 3, magnifier on | 14 | `d18ca4ad316311a4aad91503800402f1873c927c3cadc575b3b1a1ab669d196e` | `tv-tape-hm-mag-diff-v4-d18ca4ad` |
| `hm-mag-diff-v5` | process_orders_on_close, slippage 2, pyramiding 3, magnifier on | 14 | `d56124ea28d8747d3c4ce6b965bcea59f0edf42e43ffefa1484f5b36ee0eb20e` | `tv-tape-hm-mag-diff-v5-d56124ea` |
| `hm-mag-diff-v3` | process_orders_on_close, calc_on_order_fills, magnifier on | 12 | `23a25293ac35ce433214a7719e26cc58cf90471e5848bb7610e7485f1b3d079b` | `tv-tape-hm-mag-diff-v3-23a25293` |
| `hm-chart-diff-v0` | plain, magnifier off | 12 | `b2652feb01b83236315a92112acb5b108626d11d959ed4b17effc6087454ae2c` | `tv-tape-hm-chart-diff-v0-b2652feb` |
| `hm-chart-diff-v1` | process_orders_on_close, magnifier off | 12 | `27174a6b560b84c0c8f7ce541b66a6e0db648d4c1880d684059b6b050087d12c` | `tv-tape-hm-chart-diff-v1-27174a6b` |
| `hm-chart-diff-v4` | slippage 2, pyramiding 3, magnifier off | 14 | `5293cc0c2bc88dabba65ff73a024d88b28292845c537f0f4ccf58922e4d502a1` | `tv-tape-hm-chart-diff-v4-5293cc0c` |
| `hm-chart-diff-v5` | process_orders_on_close, slippage 2, pyramiding 3, magnifier off | 14 | `0080877d820ae0df81ec7eedab24b7d3a20df9f819f1e6101ff51b6049a6b44d` | `tv-tape-hm-chart-diff-v5-0080877d` |
| `hm-chart-diff-v2` | calc_on_order_fills, magnifier off | 12 | `3b3fb4c3dee2309df5f6340b5c9aef2580e62787020e82bbde8ec77fe8d4a5a3` | `tv-tape-hm-chart-diff-v2-3b3fb4c3` |
| `hm-chart-diff-v3` | process_orders_on_close, calc_on_order_fills, magnifier off | 12 | `4d45cbcd76d2949a706469307ecba90e5e5b878d84033ac8b704ec906152cdae` | `tv-tape-hm-chart-diff-v3-4d45cbcd` |
| `hm-chart-diff-v7` | process_orders_on_close, calc_on_order_fills, slippage 2, pyramiding 3, magnifier off | 14 | `f5a88e1e51e749a95eef40b094fe44b4637f4426e2b43796a9f7c5242457c581` | `tv-tape-hm-chart-diff-v7-f5a88e1e` |

TradingView dates every magnified fill at its chart bar's open; the rows are
compared at chart-bar granularity. Excursions are not compared.

`hm-mag-diff-v3` (added by R5 lane PAR-ORDERS from H-MEASURE's evidence,
`exec/H-MEASURE-scratch/agg/tapes`) witnesses the pyramiding cap: 12 rows and a
flat book, where the engine once opened 85 lots on one bar with pyramiding=1.
`hm-{mag,chart}-diff-v1` and `-v5`, `hm-chart-diff-v3` and `-v7` (the same lane,
the same source) witness the process_orders_on_close stop entry that its placing
close already reached: TradingView fills it at that close on every path, with or
without calc_on_order_fills.

`hm-chart-diff-v2` (added by R5 lane PAR-ORDERS-2 from lane PAR-ORDERS' evidence,
`exec/PAR-ORDERS-scratch/mag/tapes`) and row 5 of `hm-chart-diff-v3` / `-v7` witness
H-MEASURE Finding 6d: the market entry a calc_on_order_fills recalculation places at
the matcher's fill on bar 19's high fills at that high, with or without
process_orders_on_close.
