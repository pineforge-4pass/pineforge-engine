# Leveraged entry-bar margin tapes (R5 lane H-MEASURE, row M7 / G2-09)

TradingView's own trades for one question AUDIT4 §6 left "ruled, unmeasured":
does TradingView check a LEVERAGED opening (margin below 100 %) on the bar its
entry fills, where the adapter's `margin_check_allowed` refuses the kernel's
AfterApplied point? `tests/test_adapter_margin_schedule_differential.cpp`
(section "M7 on tapes") replays every tape through the Pine adapter and through
a bare kernel host with TradingView's money and slice. TradingView does, on 4
of 4; since R5 lane PAR-MARGIN `on_applied` admits that point for a leveraged
opening's entry bar, and the section asserts the adapter books each tape's
rows (lane H-MEASURE had pinned the adapter's late or missing call instead).

Each directory is one `lab tv` export (pineforge-workflow, channel
`ws-report-v1`, `rangeProof: covered`), byte-identical: `strategy.pine`,
`tv_trades.csv` (times UTC+8), `meta.json`, `metrics.json`. The sha256 of each
tape is `metrics.json` `tvTradesCsvHash`; the campaign note is
`tv-tape-<slug>-<first 8 hex>`.

| probe | chart | margin, size | entry bar (UTC) | TradingView |
|---|---|---|---|---|
| `hm-m7a-eth-m5-k16-0621` | BINANCE:ETHUSDT.P 15 | 5 %, 16 x equity | 2025-06-21 21:30 | margin call 6.842 @2230 (the entry bar's low), the whole book |
| `hm-m7a-eth-m5-k16-0922` | same | 5 %, 16 x | 2025-09-22 06:00 | margin call 3.793 @4000.88, the whole book |
| `hm-m7a-eth-m5-k16-1121` | same | 5 %, 16 x | 2025-11-21 07:30 | margin call 5.806 @2642.65, the whole book |
| `hm-m7a-eth-m20-k4-1010` | same | 20 %, 4 x | 2025-10-10 21:15 | margin call 0.4544 @3400 (4 x 0.1136), the rest 0.6196 at the timeout @3895 |

All four export windows are 2025-04-01 .. 2026-05-01; each script trades once.
`bars.inc` holds each event's signal bar through the entry bar + 4, from the
corpus feed `corpus/data/derived/ohlcv_ETH-USDT-USDT_15m.csv` materialized by
`scripts/derive_corpus_feeds.py` at corpus gitlink eede4a2.

## M10: the book a default-percent stop entry above 100 % opens (R5 lane PAR-MARGIN)

Eight more `lab tv` exports on NYSE:F 15m, same channel and layout, margin 25,
one buy stop placed on the session's last bar (15:45 America/New_York) and
filled at the next session's open, closed 3 bars after its entry bar:
`pm-m10-f-k4-*` at 400 % with the stop at 0.9 x close (marketable when
placed), `pm-m10-f-gapup-*` at 390 % with the stop at round(1.002 x close, 2)
(not marketable when placed, gapped through by the open). TradingView opens the
quotient of the signal close on its tick for the first and of the snapped
stop level for the second, never the fill's, and books its margin calls on
that book. `m10_bars.inc` holds each event's bars as TradingView reports them,
read by the two `pm-m10-f-ohlc*` exports (each bar printed as an order
comment); they equal the lab feed `80f404ae85ef`, half cents included.
`tests/test_adapter_margin_schedule_differential.cpp` (section "M10 on tapes")
replays all eight. Campaign notes: `tv-tape-<slug>-<first 8 hex>`.

| probe | window | TradingView | tvTradesCsvHash |
|---|---|---|---|
| `pm-m10-f-k4-0402` | 2026-03-25 .. 2026-04-07 | book 3427; margin calls 176 @11.38 | `3bd3141a7089f30c` |
| `pm-m10-f-k4-0410` | 2025-04-02 .. 2025-04-15 | book 4219; margin calls none | `8911072fc9bfb7ad` |
| `pm-m10-f-k4-1007` | 2025-09-29 .. 2025-10-10 | book 3144; margin calls 980 @12.11 | `bd8d6bc27e858bfb` |
| `pm-m10-f-k4-0309` | 2026-03-02 .. 2026-03-12 | book 3294; margin calls 4 @11.83, 56 @11.81 | `ab895fec3d476066` |
| `pm-m10-f-gapup-0501` | 2025-04-24 .. 2025-05-06 | book 3896; margin calls 376 @10.03 | `c672eb182acd2a16` |
| `pm-m10-f-gapup-0331` | 2026-03-24 .. 2026-04-03 | book 3475; margin calls 156 @11.28 | `8f54192f161b9013` |
| `pm-m10-f-gapup-0424` | 2025-04-17 .. 2025-04-29 | book 3987; margin calls 92 @9.81 | `79a6aa531e7bfbb3` |
| `pm-m10-f-gapup-0527` | 2025-05-19 .. 2025-05-30 | book 3757; margin calls 204 @10.35 | `f6222f524d46cd37` |
| `pm-m10-f-ohlc` | 2025-04-01 .. 2026-04-10 | 24 bars' open, high, low and close printed as entry comments | `8b006a00aff01690` |
| `pm-m10-f-ohlc2` | 2025-04-01 .. 2026-04-10 | 24 bars' open, high, low and close printed as entry comments | `52160db7208aea53` |

## PAR-MARGIN-2: the M7 shapes PAR-MARGIN left unmeasured (R5 lane PAR-MARGIN-2)

Thirty-six more `lab tv` exports on BINANCE:ETHUSDT.P 15m (same channel and
layout), each the lane's own synthetic script: one leveraged entry (or an entry
and an add) sized `K x strategy.equity / close` on a 0.001 grid at a signal
bar, closed 3 bars (4 for the adds) after the first open trade's entry bar.
`tests/test_adapter_margin_schedule_differential.cpp` (section "M7 shapes")
replays every one FROM ITS OWN `strategy.pine` (parsed, not restated), and
the adapter books TradingView's rows on all of them:

- `pm2-m7-coof-*`: M7-A's four scripts under `calc_on_order_fills` -- the same
  rows as `hm-m7a-*` (the same tape hashes): the call on the entry bar at its low.
- `pm2-m7-lim-*`: a limit opening (margin 5, `limit = math.round(close * 0.997, 2)`)
  filled on the next bar on its way down, whose low then breaches: the call on
  that bar at its LOW -- three high-first bars and a low-first one (0402-1945).
- `pm2-m7-slim-*` and `pm2-m7-s1lim-*`: a SHORT limit opening
  (`limit = math.round(close * 1.003, 2)`) filled on the next bar on its way UP,
  whose high then breaches, leveraged (margin 5) and at full margin (1 x): the
  call on that bar at its HIGH -- three low-first bars and a high-first one
  (0509-1100).
- `pm2-m7-poocl-*`: the four `pm2-m7-lim-*` scripts under
  `process_orders_on_close`: the same calls.
- `pm2-m7-poocm-*`: a `process_orders_on_close` market opening filled at the
  signal close; the call on the next bar at its low.
- `pm2-m7b-lim-*`: a carried long (margin 5, 4 x equity, never near its own
  line) and a LIMIT add (about 8-14 x) filled on the next bar on its way down:
  the call on the add's bar at its low, sized on the combined book, the
  first-in lot (L1) closed first.
- `pm2-m7b-mkt-*`: a carried 1 x long (margin 20) and a market add at the next
  open: the call on the add's bar at its low, on the combined book.
- `pm2-m7-mag-*`: M7-A's four scripts under the bar magnifier: the call at the
  first crossing low of TradingView's intrabars (see `../intrabar_margin`).

`pm2_bars.inc` holds each event's bars from the corpus feed
`corpus/data/ohlcv_ETH-USDT-USDT_1m.csv` (15m bars aggregated from it as the
derived feed does; the magnified scripts' one-minute bars as they are).

| probe | window | TradingView's margin calls | tvTradesCsvHash |
|---|---|---|---|
| `pm2-m7-coof-0621` | 2025-04-01 .. 2026-05-01 | 6.842 @2230 | `82afdec0f73135df` |
| `pm2-m7-coof-0922` | 2025-04-01 .. 2026-05-01 | 3.793 @4000.88 | `a56518c2145a0aa7` |
| `pm2-m7-coof-1010` | 2025-04-01 .. 2026-05-01 | 0.4544 @3400 | `5904c2624a0219b9` |
| `pm2-m7-coof-1121` | 2025-04-01 .. 2026-05-01 | 5.806 @2642.65 | `9f1b6f0dc175e472` |
| `pm2-m7-lim-0402-1945` | 2025-03-30 .. 2025-04-05 | 4.516 @1881 | `c8d52c248bfae6ad` |
| `pm2-m7-lim-0402-2000` | 2025-03-30 .. 2025-04-05 | 3.7668 @1872.48 | `c50706a1de9fa157` |
| `pm2-m7-lim-0801-0030` | 2025-07-29 .. 2025-08-04 | 2.22 @3613.67 | `07b081a7560f112c` |
| `pm2-m7-lim-1201-0215` | 2025-11-28 .. 2025-12-04 | 1.7024 @2824.6 | `8dbe22c6e8867a97` |
| `pm2-m7-mag-0621` | 2025-06-19 .. 2025-06-23 | 6.842 @2230 | `82afdec0f73135df` |
| `pm2-m7-mag-0922` | 2025-09-20 .. 2025-09-24 | 3.793 @4076.79 | `5c3468e8176b5aad` |
| `pm2-m7-mag-1010` | 2025-10-08 .. 2025-10-12 | 0.3096 @3428.11 | `bbac24deb07eed25` |
| `pm2-m7-mag-1121` | 2025-11-19 .. 2025-11-23 | 5.806 @2680.37 | `d2ebdc4cff50a62c` |
| `pm2-m7-poocl-0402-1945` | 2025-03-30 .. 2025-04-05 | 4.516 @1881 | `e7852d9c3904147c` |
| `pm2-m7-poocl-0402-2000` | 2025-03-30 .. 2025-04-05 | 3.7668 @1872.48 | `0d4698a024f7cbf8` |
| `pm2-m7-poocl-0801-0030` | 2025-07-29 .. 2025-08-04 | 2.22 @3613.67 | `816a9c1fda6dff83` |
| `pm2-m7-poocl-1201-0215` | 2025-11-28 .. 2025-12-04 | 1.7024 @2824.6 | `b6332c9af3571acf` |
| `pm2-m7-poocm-0105-1415` | 2026-01-02 .. 2026-01-08 | 1.496 @3132.58 | `b0ae1e3b89dcf62c` |
| `pm2-m7-poocm-0402-1945` | 2025-03-30 .. 2025-04-05 | 5.4568 @1881 | `b7e708608a5cea37` |
| `pm2-m7-poocm-0702-0100` | 2025-06-29 .. 2025-07-05 | 3.9452 @2367.53 | `78ad71bf244a719e` |
| `pm2-m7-poocm-1002-1445` | 2025-09-29 .. 2025-10-05 | 1.4844 @4332.23 | `3c75ca7c5cd23668` |
| `pm2-m7-s1lim-0402-1330` | 2025-03-30 .. 2025-04-05 | 0.0152 @1892.7 | `d633d75493717e91` |
| `pm2-m7-s1lim-0509-1100` | 2025-05-06 .. 2025-05-12 | 0.0132 @2375.39 | `5f62b5db446e583a` |
| `pm2-m7-s1lim-0709-1930` | 2025-07-06 .. 2025-07-12 | 0.0144 @2795.73 | `8d7299c2d5e6ae4f` |
| `pm2-m7-s1lim-1001-0830` | 2025-09-28 .. 2025-10-04 | 0.0096 @4329 | `8981c3291ddf2d14` |
| `pm2-m7-slim-0402-1330` | 2025-03-30 .. 2025-04-05 | 3.2528 @1892.7 | `1d5f09118992a3fd` |
| `pm2-m7-slim-0509-1100` | 2025-05-06 .. 2025-05-12 | 2.6232 @2375.39 | `8b448336731119f7` |
| `pm2-m7-slim-0709-1930` | 2025-07-06 .. 2025-07-12 | 2.5484 @2795.73 | `02f292a474aef979` |
| `pm2-m7-slim-1001-0830` | 2025-09-28 .. 2025-10-04 | 1.5688 @4329 | `320a3e2199cfaa91` |
| `pm2-m7b-lim-0402-1930` | 2025-03-30 .. 2025-04-05 | 2.099 @1881, 2.4142 @1881 | `e6aaeb8a522871f6` |
| `pm2-m7b-lim-0402-1945` | 2025-03-30 .. 2025-04-05 | 2.093 @1872.48, 1.8338 @1872.48 | `785ea45bf94a33ee` |
| `pm2-m7b-lim-0801-0015` | 2025-07-29 .. 2025-08-04 | 1.084 @3613.67, 1.042 @3613.67 | `0fcd4f3a0df2f3cb` |
| `pm2-m7b-lim-1207-1345` | 2025-12-04 .. 2025-12-10 | 1.32 @2903.75, 0.5228 @2903.75 | `b0e782a34b812dd2` |
| `pm2-m7b-mkt-0105-1400` | 2026-01-02 .. 2026-01-08 | 0.084 @3132.58 | `080f04615748c335` |
| `pm2-m7b-mkt-0402-1930` | 2025-03-30 .. 2025-04-05 | 0.3136 @1881 | `a5f4d89f44b0fa93` |
| `pm2-m7b-mkt-0702-0045` | 2025-06-29 .. 2025-07-05 | 0.2288 @2367.53 | `cf8b4bacde9150a9` |
| `pm2-m7b-mkt-1002-1430` | 2025-09-29 .. 2025-10-05 | 0.084 @4332.23 | `c2c01658ce472bb6` |
