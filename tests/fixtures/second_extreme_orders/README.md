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

The `pa3-f4-*` tapes (R5 lane PAR-ORDERS-3, finding 4) carry the rule to a bar's FIRST
extreme: lot A's limit entry exactly on it (long j = 4, 25, 40; short j = 2, 44), and the
recalculation A's fill starts places a market entry B, strategy.close("A"), close_all, a
market strategy.order, A's exit stop already through A's price, or a stop entry B between
the extremes (pyramiding 2); a STOP entry A exactly on it with the market add B, and E's
EXIT limit X exactly on it with the market re-entry (and that re-entry's own exit limit
X2 already through) -- the two shapes ab9714be's rows pin (long j = 2, 32, 44 on high-first
bars; short j = 4, 25, 40 on low-first ones); and the control, a point fill forced onto the
first extreme (P's stop exit on the leg to it, the market entry A, then B).

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
| `pa3-f4-level-w1-mkt-long` | level fill on the first extreme, market entry B, long | 6 | `3a1d2405c1917c4b952affb50f59479af56218cc82401a20d8216860dc05b2ae` | `tv-tape-pa3-f4-level-w1-mkt-long-3a1d2405` |
| `pa3-f4-level-w1-mkt-short` | the same, short | 4 | `fd73b977c8688f0e96f2e48b7ee1ed3c303dbf2b2c2977ea400536f50981ac24` | `tv-tape-pa3-f4-level-w1-mkt-short-fd73b977` |
| `pa3-f4-level-w1-close-long` | level fill on the first extreme, strategy.close("A"), long | 3 | `749d04f062676b2651d347b52467b4f912f472defa39657e3a3caf8ffa23a02a` | `tv-tape-pa3-f4-level-w1-close-long-749d04f0` |
| `pa3-f4-level-w1-close-short` | the same, short | 2 | `b2b84ed4a42804cf96449f6039c82af7f9c216984de25c20bd31aef912345a8a` | `tv-tape-pa3-f4-level-w1-close-short-b2b84ed4` |
| `pa3-f4-level-w1-closeall-long` | level fill on the first extreme, strategy.close_all(), long | 3 | `2b6a49e04cfd2624dd5606a5a665e49ae779b65e500af722c077c7392d3374b7` | `tv-tape-pa3-f4-level-w1-closeall-long-2b6a49e0` |
| `pa3-f4-level-w1-closeall-short` | the same, short | 2 | `d4aed65ee4962cd54cc3508165345d9efdcd3e7f9c9ec59ac93061c8a1b7f35b` | `tv-tape-pa3-f4-level-w1-closeall-short-d4aed65e` |
| `pa3-f4-level-w1-order-long` | level fill on the first extreme, market strategy.order B, long | 6 | `3a1d2405c1917c4b952affb50f59479af56218cc82401a20d8216860dc05b2ae` | `tv-tape-pa3-f4-level-w1-order-long-3a1d2405` |
| `pa3-f4-level-w1-order-short` | the same, short | 4 | `fd73b977c8688f0e96f2e48b7ee1ed3c303dbf2b2c2977ea400536f50981ac24` | `tv-tape-pa3-f4-level-w1-order-short-fd73b977` |
| `pa3-f4-level-w1-xstop-long` | level fill on the first extreme, A's exit stop already through, long | 3 | `049be702a2c8a5f8c20df4e77e4ea1872881953d54201a02cb7626f9b45dd729` | `tv-tape-pa3-f4-level-w1-xstop-long-049be702` |
| `pa3-f4-level-w1-xstop-short` | the same, short | 2 | `15afb3ed286c0119dcd2d1a3ab7720a7f607d4a6380e226e4f42a2a7301d8d35` | `tv-tape-pa3-f4-level-w1-xstop-short-15afb3ed` |
| `pa3-f4-level-w1-bstop-long` | level fill on the first extreme, stop entry B between the extremes, long | 6 | `36f906204c94f80a15cbd599c1b0464640d29da7b40756338ece0bed280608de` | `tv-tape-pa3-f4-level-w1-bstop-long-36f90620` |
| `pa3-f4-level-w1-bstop-short` | the same, short | 4 | `d928448f99563acb3db1d51515a57c300b791f68ca63c27f5a782af3ea09fb7c` | `tv-tape-pa3-f4-level-w1-bstop-short-d928448f` |
| `pa3-f4-stop-w1-mkt-long` | stop entry A exactly on the first extreme, market entry B, long | 6 | `61e651ebb9a9cb8409981292eab3e2ddcf0dc85996b20f414c4842ecd83fd12e` | `tv-tape-pa3-f4-stop-w1-mkt-long-61e651eb` |
| `pa3-f4-stop-w1-mkt-short` | the same, short | 6 | `831409ef2c4ec829e3067ed262d9d881b94fbc45f16ad9794e29506747b35d75` | `tv-tape-pa3-f4-stop-w1-mkt-short-831409ef` |
| `pa3-f4-xlimit-w1-reentry-long` | E's exit limit X exactly on the first extreme, market re-entry, long | 6 | `056d2b5de34c807a1af837f6e312b7cfef04abbab8eb0d3d265fac0984f23aba` | `tv-tape-pa3-f4-xlimit-w1-reentry-long-056d2b5d` |
| `pa3-f4-xlimit-w1-reentry-short` | the same, short | 6 | `a3205f035e9216bd0d30e74a8bbd6e70654b7486cbc041a66cf1bfba69455dda` | `tv-tape-pa3-f4-xlimit-w1-reentry-short-a3205f03` |
| `pa3-f4-xlimit-w1-reentry-tp-long` | the same, and the re-entry's exit limit X2 already through, long | 6 | `b44f5702fe7b13f6e67cb56dc9d9be378057529a1f96da45d3cdfb34c1d6f0ec` | `tv-tape-pa3-f4-xlimit-w1-reentry-tp-long-b44f5702` |
| `pa3-f4-xlimit-w1-reentry-tp-short` | the same, short | 6 | `3c4561c2b5a4fe900e368c14b244ea8019be64cc95f6f90b7294dcb5a6abd90b` | `tv-tape-pa3-f4-xlimit-w1-reentry-tp-short-3c4561c2` |
| `pa3-f4-point-w1-mkt-long` | point fill forced onto the first extreme, market entry B, long | 6 | `4a0c3d450ec01bb70b6e8341391e9babf4319a7eb53316a523d7c7cf3a323d35` | `tv-tape-pa3-f4-point-w1-mkt-long-4a0c3d45` |
| `pa3-f4-point-w1-mkt-short` | the same, short | 6 | `b85b054c0c53200c9db1f8fd5c0c1bb6008bb38277034ed05ba0d5f3155666ff` | `tv-tape-pa3-f4-point-w1-mkt-short-b85b054c` |
