# process_orders_on_close limit close tapes (R5 lane PAR-ORDERS-3, finding 3)

TradingView's own trades for a limit entry a `process_orders_on_close` script places at a
bar's close calculation, flat, at a level that bar's close tick or its range reaches; the
entry is cancelled and the book closed on bar j+2. Sell limits: bar 22 (close 11.755, high
11.755) at 11.76; bar 27 (close 11.845, high 11.95) at 11.86; bar 31 (close 11.855, high
11.87) at 11.86; bar 49 (close 11.595, high 11.625) at 11.61. Buy limits: bar 13 (close
11.655, low 11.655) at 11.65; bar 23 (close 11.745, low 11.745) at 11.74; bar 38 (close
11.805, low 11.79) at 11.79; bar 46 (close 11.62, low 11.61) at 11.61. TradingView fills
bars 22 and 31 at their close -- the close's tick, 11.76 / 11.86, reaches the limit -- and
every other entry on the next bar, with and without `calc_on_order_fills`.
`tests/test_pooc_limit_close_tapes.cpp` replays each tape through the Pine adapter on the
15m chart.

Each directory is one `lab tv` export (pineforge-workflow, channel `ws-report-v1`,
`rangeProof: covered`, NYSE:F 15, window 2025-07-01 .. 2025-07-08), byte-identical:
`strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The
scripts count chart bars from 2025-07-02 09:30 ET, the first bar of
`tests/fixtures/session_islastbar/bars.inc`, whose 15m bars the rows replay.

| probe | variant | trades | tvTradesSha256 | note |
|---|---|---|---|---|
| `pa3-f3-pooc-limit-long` | buy limits, process_orders_on_close | 4 | `82248d1f8a801e970fdb2d369ec4c6ec63f885ad6682db4b2e5eabffdd7b61c4` | `tv-tape-pa3-f3-pooc-limit-long-82248d1f` |
| `pa3-f3-pooc-limit-long-coof` | the same, calc_on_order_fills | 4 | `82248d1f8a801e970fdb2d369ec4c6ec63f885ad6682db4b2e5eabffdd7b61c4` | `tv-tape-pa3-f3-pooc-limit-long-coof-82248d1f` |
| `pa3-f3-pooc-limit-short` | sell limits, process_orders_on_close | 4 | `5590f99e59826d8983e5220f89871077ea6d4c624330c2c4e713e8e3686ffa1f` | `tv-tape-pa3-f3-pooc-limit-short-5590f99e` |
| `pa3-f3-pooc-limit-short-coof` | the same, calc_on_order_fills | 4 | `5590f99e59826d8983e5220f89871077ea6d4c624330c2c4e713e8e3686ffa1f` | `tv-tape-pa3-f3-pooc-limit-short-coof-5590f99e` |
