# A resting entry's bracket, re-issued by its fill's recalculation, keeps its stop live (R5 lane TAIL-D)

Under `calc_on_order_fills` a limit entry E that fills inside a leg starts a recalculation there.
The bracket X the script placed while E rested becomes live at E's fill, and TradingView fills its
stop at its level on the rest of that leg, also when the recalculation re-issues X with its limit
moved or unchanged and the stop the same. The engine held the re-issued stop to the leg's end and
booked the extreme (mehmettopbas-ict-10am-first-fvg on NSE:NIFTY 15).

Each directory is one `lab tv --no-note` export of the lane's own synthetic script (maintainers' private pineforge-workflow, channel `ws-report-v1`, BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-04-05), byte for byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json` (`tvTradesCsvHash` is the sha256 of `tv_trades.csv`), `meta.json`. No closed or scraped strategy is involved.
`tests/test_coof_resting_bracket_tapes.cpp` replays every tape through the Pine adapter.

| tape | shape | trades | tv_trades.csv sha256 | rangeProof |
|---|---|---:|---|---|
| `td-m2a` | E a limit 2 points through the close with X (stop 3, limit 20 beyond); in the position X is re-issued with its limit at 10 | 37 | `1d22b3a7ccfaa416d2a1cd30cc46bb03db3679b328c5715123f8332936a2ea7f` | covered |
| `td-m2b` | X re-issued unchanged | 37 | `65ea647efc884815dc9b3f0d12b746f4412610233462c655dcefccef4dcb691e` | covered |
| `td-m2c` | X not re-issued (control) | 37 | `65ea647efc884815dc9b3f0d12b746f4412610233462c655dcefccef4dcb691e` | covered |

## Bars

`bars.inc`: 2025-04-01 00:00 .. 2025-04-04 23:45 UTC of the corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`).
