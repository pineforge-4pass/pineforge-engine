# Margin calls and entry admission at Pine v6's defaults (lane W5-ENG-MARGIN-V6)

Pine v6's default order is 100% of equity and its default margin is 100% both
ways (lane TV-DEFAULTS, `tests/fixtures/tv_strategy_defaults`), so a strategy's
margin calls and TradingView's entry admission now decide most of its trades.
The Pine adapter rules below were missing or wrong under those defaults. Each
is pinned here by synthetic probes written for this lane; no closed or scraped
strategy is involved.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe but the two NYSE:F ones runs on
BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-06, with fixed
`timestamp("UTC", ...)` cells and a cleanup `strategy.close_all()` after each;
none declares `initial_capital`, so each runs on v6's 100000. The two NYSE:F
probes run on NYSE:F 15, 2025-04-15 .. 2025-05-20, the chart and shape of
`badapter-v19dp1-control` (`tests/test_adapter_margin_revival_cancel.cpp`).

`tests/test_margin_v6_tapes.cpp` replays every tape through the Pine adapter
under the configuration the generated constructor declares for that probe, over
the corpus 15m bars in `bars.inc` (2025-04-01 00:00 .. 2025-04-17 00:00 UTC) or
the NYSE:F 15m bars of `tests/test_adapter_margin_revival_cancel_data.hpp`,
with TradingView's lot (0.0001 ETH, one NYSE:F share) as the `qty_step`, and
requires each trade the tape closes inside the replayed bars -- entry and exit
time, side, price in ticks of 0.01, quantity in lots -- to be the engine's. On
the lane's base (8633d944) every rule tape below fails there and every control
passes.

## MR: a from_entry="" exit is the held position's

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5-mr-global` | 10 | nothing (v6 defaults) | six shorts, each reversed on its fill bar by a default long, with a `from_entry=""` exit re-issued every bar while short and already through at the next open; where that open declines the long, TradingView books exactly the trades of the named-exit control | `97e76f2af327dba9dc262c4202329bec3fe0f2f52389f472d7ac89fe3c9368ef` |
| `w5-mr-named` | 10 | nothing (v6 defaults) | the control: the same exit named `from_entry="S"`; the same file | `97e76f2af327dba9dc262c4202329bec3fe0f2f52389f472d7ac89fe3c9368ef` |
| `w5-mr-once-global` | 10 | nothing (v6 defaults) | four of the same cells, the exit placed once, with the reversal: the same as its named control | `9866b6fcf414cdbeb1b84c8a6d2d1afcbf1c34f47833fa5d145a2e76a18da6d5` |
| `w5-mr-once-named` | 10 | nothing (v6 defaults) | the control of `w5-mr-once-global`; the same file | `9866b6fcf414cdbeb1b84c8a6d2d1afcbf1c34f47833fa5d145a2e76a18da6d5` |
| `w5-rv-global-held` | 2 | `initial_capital=10000`, percent 100, margin 100 | the V19D-P1 shape with the stop exit placed `from_entry=""` on the bar after the short fills: the 05-02 09:30 margin call (12 @10.39) revives it and it closes the rest (928 @10.39) | `bdf3d470c8b75f9c53f1ffe0872ef90d2d016657fe74c9b3716172f28a9b68a2` |
| `w5-rv-named-control` | 2 | the same | the control: the exit named `"S"` with the entry; the same file | `bdf3d470c8b75f9c53f1ffe0872ef90d2d016657fe74c9b3716172f28a9b68a2` |

## M1: an add is admitted on its combined margin

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5-m1-addexit-p100` | 8 | `pyramiding=10` | each same-side default add, placed with an exit the next open is already through, is dropped at placement: the held side plus the add need twice the equity, and the exit alone fills at that open | `422ca54ad24b341cf82046b689f4d4ced6ee4e7409759d226020567fdfabab41` |
| `w5-m1-addexit-p60` | 5 | `pyramiding=10`, percent 60 | the control: 60% + 60% exceeds the equity too, so every add is dropped | `3058803d4ca04a0beb3c67d90ee4be5aa5ac1eafbbdc609323fb1fcfe328ad1b` |
| `w5-m1-addclose` | 4 | `pyramiding=2` | an add followed on the same bar by `strategy.close` of the held id (A, and C on a short) or by `strategy.close_all` (B): the close empties the book at the next open and the add never opens | `a2ccf3a3af3ee4fb11770b78317c3eb942eb012021455f75f7fae81cd74dd0f0` |

The lowering let the add reach the kernel; where the exit's leg or the close
took the open first, the add then opened a new position from flat. An add
whose shortfall stays within one lot is still admitted, as the fill-time add
arm admits it (`tests/test_reversal_admission_float_guard_l4c.cpp` pin E).

## C1: a carried process_orders_on_close short is called before the script

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5-m2-pooc-short-comm` | 9 | `process_orders_on_close`, commission 0.05 %, slippage 1 | three shorts filled at a signal close; where the next bar opens above the fill and rises further, a call at that open and another at the same bar's high | `cf1395a929bf34f4f047c91f4673c7462950608a862193a9f4fddd9c25f10d53` |
| `w5-c1-seen-size` | 5 | the same | the 12:00 short, called at the 12:15 open and high and again at the 12:30 high (1.5312 @1802.01); a reversal on that bar by `math.abs(strategy.position_size)` opens 53.645, the size after the call | `fa7efd0c7eaa7505b7f0902ff69c34f2ba9bf652df91805879cbcb4b92be09b9` |
| `w5-m2-pooc-carried-close` | 4 | the same | the same calls, then `strategy.close_all` at the close takes the 53.645 left | `890535b85a21b448d0ccdfd8cb9651f935e713b9b8e699b82c2414c06daa007e` |
| `w5-m2-pooc-carried-none` | 4 | the same | the same calls with nothing at the close but a `strategy.cancel_all()` | `018d0f5213132630c7c2af15b522673f91c524ee41143d53516b0948308ac187` |
| `w5-m2-pooc-carried-reverse` | 6 | the same | the same calls, then a default reversal sized from the equity after them; the new long's call follows at the next open (C2) | `a89b97c89518f4ff40a5ba72154085994302e4a7ae8599f86ee29131a96cf423` |

ab9714be checked a commissioned or slipped carried short only after the
script, not at all once a slice at the bar's open had been taken, and deferred
the check behind a close-time market order to the post-fill book: a close_all
or a reversal consumed the short with no call, and a reversal was sized from
the equity before it. Each carried tape also needs the 12:15 opening call
(M2), and `w5-m2-pooc-carried-reverse` its new long's call (C2): the test
replays them from those rules on.

## Bars

`bars.inc` is the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`),
rows 2025-04-01 00:00 .. 2025-04-17 00:00 UTC copied as text. Trades the tapes
close after its last bar (the cells of 2025-04-18 and later) are not replayed.
