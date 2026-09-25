# Pending-entry trailing-exit tapes (H-MEASURE rows E5 pending-entry arm, E14 anchored-leg seed)

TradingView's own trades for a `strategy.exit(trail_points=n, trail_offset=k)`
issued on the SIGNAL bar together with the `strategy.entry` it names, so the
exit is placed while its entry is still pending: a MARKET entry that fills at
the next bar's open, or (the two `-limit-` tapes) a LIMIT entry that fills
mid-bar one or two bars later. The Pine adapter lowers that exit to the kernel's anchored
child (`relative_leg_shapes`, armed through `resolve_anchored_level`) and
adopts it at the fill. `tests/test_pending_entry_trail_tapes.cpp` replays every
trade below through the adapter and reads the kernel's record.

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`, NYSE:F 15, 2025-04-01 .. 2026-04-10),
byte-identical: `strategy.pine`, `tv_trades.csv` (times UTC+8), `meta.json`,
`metrics.json`.

| probe | trades | tvTradesSha256 | TradingView | adapter |
|---|---|---|---|---|
| `hm-orders-h4-f-long-pending-arm` | 8 | `620a55e297f6bd8b40a4a9ed04195f456bbeb24e2f02468a0fe29fa2ced4b9a3` | the arm bar's raw high stops inside the activation's tick cell (10.175, 10.305, 10.615, 10.125, 10.325, 11.899, 13.049, 13.419); every trade exits on that bar at activation - 1 tick (seven on the fill bar itself) | 8/8 |
| `hm-orders-h4-f-short-pending-arm` | 8 | `41d1837adb0ebcc096e544de4abbd65eb2b0abf56553257ae2e004380a221521` | the short twin (lows 9.995, 9.915, 10.145, 10.165, 9.895, 13.041, 13.665, 13.665): activation + 1 tick on the arm bar (four on the fill bar) | 8/8 |
| `hm-orders-h4-f-long-pending-limit-arm` | 6 | `886d8cc2e2caa9134073cbb0910121aaa844824e66e9170e6615ab7128cb6c13` | the entry is a LIMIT (`strategy.entry(limit=X)`, issued with the exit) that fills mid-bar one or two bars later; the high after the fill stops in the activation's cell (10.585, 11.455, 11.635, 12.515, 12.935, 14.055) and every trade exits on that same fill bar at activation - 1 tick | 6/6 |
| `hm-orders-h4-f-short-pending-limit-arm` | 6 | `f88f872fe583f0b85a33909f5bc113fbd3e0c529798f5c21f2e359b95501cf2c` | the short twin (lows 9.825, 9.905, 10.165, 10.165, 9.895, 13.041; arms 0 to 3 bars after the fill): activation + 1 tick on the arm bar | 6/6 |
| `hm-orders-h3-f-long-pending-seed` | 8 | `95b1e8309f3b2f93645f36d68b3b479517b7c303d0b7330b50622364a0787252` | E14's shape with the exit on the signal bar: the fill bar's high reaches the activation only on its tick, the next bar is shallow; every trade exits on that next bar at activation - k ticks (k 3 or 5): the running best starts AT the activation | 0/8 (recorded: exits 5 to 24 bars later) |
| `hm-orders-h3-f-short-pending-seed` | 5 | `03654a5b3f98b45be25f6c9be212943a43ad4e1683c5d4b7d126567ba226966a` | the short set (k 1 to 3; arms on the fill bar or later): activation + k ticks | 0/5 (recorded: 1 to 3 bars later) |

`bars.inc` holds, per event, the feed bars from the signal bar through the bar
the probe's 25-bar timeout close would fill on (28 bars; 30 for the limit
tapes, whose entry fills one or two bars after the signal), with the per-event
`trail_points`, `trail_offset` and limit level: the registry feed `lab bars NYSE:F 15`,
evidence sha256 `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`
(`lab evidence get <sha> --out <file>`). The events were chosen with the
search scripts of the H-MEASURE `orders` scratch (`pending_trail_search.py`,
`select_events.py`), which model three readings: a quantized arm with the best
seeded at the activation (TradingView: 29/29), the adapter's anchored child
(best from the raw arm print: 16/29, bit-identical to the adapter), and a raw
arm (0/29); `pending_limit_search.py` does the same for the limit tapes
(TradingView 12/12 = the adapter; raw arm 0/12).
