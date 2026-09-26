# Second-extreme order tapes (R5 lane PAR-ORDERS-2, items 2 and 4)

TradingView's own trades for orders a `calc_on_order_fills` recalculation places
exactly on a bar's SECOND extreme (H-MEASURE Finding 6d): lot A is open; lot B is a
stop entry placed exactly on bar j's second extreme (the high of a low-first bar,
the low of a high-first one), so its fill starts the recalculation there with A still
open (pyramiding 2), and that recalculation places an exit for A reached on the
extreme -> close leg, or calls `strategy.close("A")`. Long cycles j = 7, 19, 26, 42;
short j = 5, 30, 46, 51. `tests/test_second_extreme_order_tapes.cpp` replays each
tape through the Pine adapter on the 15m chart (the short tapes on their first
cycle: TradingView fills B's off-grid 11.795 sell stop on bar 38, not on bar 30).

The `pa2-i2-point-w2-*` and `pa2-i2-level-w1-*` tapes are the boundary of that rule, one
flat lot at a time (pyramiding 1): a fill booked AT the second extreme because a
recalculation's order gap-fills there (point-w2: P's stop exit fills on the leg to the first
extreme, the market entry A its recalculation places fills on that extreme, the exit AX A's
recalculation places between the extremes gap-fills at the second, and AX's recalculation
places the market entry C -- long j = 3, 41, short j = 2, 30), and a limit entry A AT the
first extreme with the same AX and C (level-w1 -- long j = 4, 25, 40, short j = 2, 32, 44).

Each directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, NYSE:F 15, window 2025-07-01 .. 2025-07-08), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The
scripts count chart bars from 2025-07-02 09:30 ET, the first bar of
`tests/fixtures/session_islastbar/bars.inc`, whose 15m and 1m bars the rows replay.

| probe | variant | trades | tvTradesSha256 | note |
|---|---|---|---|---|
| `pa2-i2-exit-w2-long` | exit AX of lot A, long, stop between close and high | 8 | `b3ccb9c57b66baf6dd130b676e52cce418a1f64b4d109cca3725616e5a313530` | `tv-tape-pa2-i2-exit-w2-long-b3ccb9c5` |
| `pa2-i2-exit-w2-long-pooc` | the same, process_orders_on_close | 8 | `1b21b85bc50a052cee2749d2745c5d67cb434c9c8eb534aa1b22648bd00abc03` | `tv-tape-pa2-i2-exit-w2-long-pooc-1b21b85b` |
| `pa2-i2-exit-w2-short` | exit AX of lot A, short, stop between low and close | 7 | `6864d39da5adfd779657801af3be47c7940783b14c4df0bc8af3e58f4624dd39` | `tv-tape-pa2-i2-exit-w2-short-6864d39d` |
| `pa2-i2-exit-w2-short-pooc` | the same, process_orders_on_close | 7 | `05f4980868362562d407a5bff8e203c87edbfe6c519fe8fdf6a4e7d646fe8fec` | `tv-tape-pa2-i2-exit-w2-short-pooc-05f49808` |
| `pa2-i4-close-w2-long` | strategy.close("A"), long | 8 | `738a5955eb9e2b4bf930b0f8e3e70a952ecebdad38900b4a5a48d43149663ee3` | `tv-tape-pa2-i4-close-w2-long-738a5955` |
| `pa2-i4-close-w2-long-pooc` | the same, process_orders_on_close | 8 | `0130ee9f496a06c6afc48d8a96634be2697158deee75b8e2db42eb1eb1a788eb` | `tv-tape-pa2-i4-close-w2-long-pooc-0130ee9f` |
| `pa2-i4-close-w2-short` | strategy.close("A"), short | 7 | `777b55c471124e7a9747058be9f362e8a3bba618c93fced1358ecdd8846b3bc6` | `tv-tape-pa2-i4-close-w2-short-777b55c4` |
| `pa2-i4-close-w2-short-pooc` | the same, process_orders_on_close | 7 | `d88c061b3786fa2ef655082c7d9893900f5419aa871586dda0041b17cddb2eac` | `tv-tape-pa2-i4-close-w2-short-pooc-d88c061b` |
| `pa2-i2-point-w2-long` | point fill on the second extreme, long | 6 | `88db2d9cd2a7f2dd4d7692007935c6e165b8476ed418fc9f293f6f691c8728ea` | `tv-tape-pa2-i2-point-w2-long-88db2d9c` |
| `pa2-i2-point-w2-long-pooc` | the same, process_orders_on_close | 6 | `1fce1bed23df68231ed7546fe4fe13760116654e945a4cf1cdb02a9d755d6ca5` | `tv-tape-pa2-i2-point-w2-long-pooc-1fce1bed` |
| `pa2-i2-point-w2-short` | point fill on the second extreme, short | 6 | `e292e70d771691a3fd44670abc9a699270d5ad677b3d480ff7402a0421fa7abd` | `tv-tape-pa2-i2-point-w2-short-e292e70d` |
| `pa2-i2-point-w2-short-pooc` | the same, process_orders_on_close | 6 | `03c75065f9522e4e339594fe6c8e703e898ef35878e4351522db6622f372fb1f` | `tv-tape-pa2-i2-point-w2-short-pooc-03c75065` |
| `pa2-i2-level-w1-long` | limit entry at the first extreme, long | 6 | `9a22c1daf2574a168815188429caaa9de881a9ff5ab637712b468828085b0499` | `tv-tape-pa2-i2-level-w1-long-9a22c1da` |
| `pa2-i2-level-w1-long-pooc` | the same, process_orders_on_close | 6 | `7f7a332d07202c8a1814a7119b9f6cd70dda335fb563d40403d1e7e23ab3be89` | `tv-tape-pa2-i2-level-w1-long-pooc-7f7a332d` |
| `pa2-i2-level-w1-short` | limit entry at the first extreme, short | 6 | `8d4893ade201e54203b7723abe267e211a26c92480620fea341d73882f12014d` | `tv-tape-pa2-i2-level-w1-short-8d4893ad` |
| `pa2-i2-level-w1-short-pooc` | the same, process_orders_on_close | 5 | `c092e35ad56281fdc114e0923e9e24e3c47ea7f7a93ddce883b2cde07d69fd99` | `tv-tape-pa2-i2-level-w1-short-pooc-c092e35a` |
