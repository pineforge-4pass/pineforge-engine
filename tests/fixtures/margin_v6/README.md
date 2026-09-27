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

## Bars

`bars.inc` is the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`),
rows 2025-04-01 00:00 .. 2025-04-17 00:00 UTC copied as text. Trades the tapes
close after its last bar (the cells of 2025-04-18 and later) are not replayed.
