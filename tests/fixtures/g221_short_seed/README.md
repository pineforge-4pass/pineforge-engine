# Short-seed collision tape (R5 wave H, lane H-MEASURE, row G2-21)

Finding 272's short-seed collision (`tests/oracle/test_oracle_short_seed_percent.cpp`)
once a day under percent-of-equity default sizing: a SHORT seed, then in one
bar `entry(Long); entry(Short); close(Long); close(Short)`, the remnant closed
from the 01:00 signal. `tests/test_short_seed_report_swap.cpp` replays it
through a `PineStrategyHost`: the rows are TradingView's, and on the one day a
remnant is closed by a close of the seed id the adapter swaps two rows' entry
incarnations (`project_short_seed_report_rows`), which TradingView does not
export.

| tape (`lab tv`, ws-report-v1, rangeProof covered) | chart | window | trades | tv_trades.csv sha256 | campaign note |
|---|---|---|---|---|---|
| `hm-g221-short-seed` | BINANCE:ETHUSDT.P 15 | 2025-06-02 .. 2025-06-06 | 13 | `9a842c022746bc393650fc3f09f363808a53f7c078c05dde00f65e408f1734e8` | `tv-tape-hm-g221-short-seed-9a842c02` |

`bars.inc` (`exec/H-MEASURE-scratch/report/row2-shortseed/gen_bars.py`): the
corpus 15m chart feed (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived
from the corpus 1m feed `db8c1332…`), contiguous 2025-06-02 00:00 ..
2025-06-05 23:45 UTC (384 bars): percent sizing carries equity from day to
day, so the days cannot be replayed apart.
