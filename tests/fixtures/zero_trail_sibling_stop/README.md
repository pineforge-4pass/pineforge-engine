# Zero-offset trail, next open ON the carried best (H-MEASURE row M16 / G2-18)

TradingView's own trades for `strategy.exit(trail_points=n, trail_offset=0)`
issued at the close of the bar whose open filled the entry, where that close
has ALREADY reached the activation (on its tick), and the next bar opens
exactly on the carried best (the close, or the activation when the close
reaches it only on its tick) and moves favourably first to a new extreme. The
Pine adapter rests two legs there: the kernel `Trail` (TrailTicks 0.5,
`best_seed` = the carried best) and a sibling `Stop` at the carried best.
`tests/test_zero_trail_sibling_stop.cpp` replays each trade through the
adapter, through a bare kernel given the adapter's Trail alone, and through a
bare kernel given both legs.

Each directory is one `lab tv` export (`ws-report-v1`, `rangeProof: covered`,
NYSE:F 15, 2025-04-01 .. 2025-09-30), byte-identical.

| probe | trades | tvTradesSha256 | TradingView | adapter | kernel Trail alone |
|---|---|---|---|---|---|
| `hm-orders-m16-f-long-zero-trail-open-on-best` | 8 | `4a5bbbd341a7565d7d99a593b20d389b000bdcf3a55960f4c5e19f0a8a7eb5c7` | every exit on the next bar at the carried best's tick (the touch at the open) | 8/8 | 0/8 |
| `hm-orders-m16-f-short-zero-trail-open-on-best` | 8 | `2e1f165d18468d7904ef034cae12f06a7491467236811d45257750a408fafbe4` | the short twin | 8/8 | 0/8 |

`bars.inc`: per event the 28 feed bars from the signal bar (registry feed
`lab bars NYSE:F 15`, evidence sha256
`80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`), with the
per-event `trail_points`.
