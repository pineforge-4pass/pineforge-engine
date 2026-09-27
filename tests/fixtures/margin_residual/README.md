# Margin residuals at Pine v6's defaults (lane W5B-ENG-MARGIN-RESIDUAL)

Pine v6's default order is 100% of equity and its default margin is 100% both
ways, so a strategy's margin calls and TradingView's entry admission decide
most of its trades. Lane W5-ENG-MARGIN-V6 (`tests/fixtures/margin_v6`) fixed
seven such rules; the Pine adapter rules below are the ones its residual
probes still diverged on. Each is pinned here by synthetic probes written for
this lane; no closed or scraped strategy is involved.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15-minute
bars, 2025-04-01 .. 2025-05-06; none declares `initial_capital`, so each runs
on v6's 100000.

`tests/test_margin_residual_tapes.cpp` replays every tape through the Pine
adapter under the configuration the generated constructor declares for that
probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
(`tests/fixtures/margin_v6/bars.inc`, 2025-04-01 00:00 .. 2025-04-17 00:00
UTC), with TradingView's lot (0.0001 ETH) as the `qty_step`, and requires each trade the tape
closes inside the replayed bars -- entry and exit time, side, price in ticks
of 0.01, quantity in lots -- to be the engine's. On the lane's base (1ed0a9e4)
every rule tape below fails there and every control passes.

## SB: a flat bar's entries all fill, and their book is called once

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-sb-market-60x2` | 120 | `pyramiding=2` | every 3 hours from flat, two same-side MARKET entries of explicit quantity 60 % of `strategy.equity`, placed on one bar: both fill at the next open, and the 120 % book is margin-called there (FIFO: all of A and part of B), sized 4 x the lot-floored shortfall | `b9ba86de453f664d08b724a9fffaad31b3e540dd17f3b6ecdf24c1e0727cf9a9` |
| `w5b-sb-pooc-60x2` | 120 | the same, `process_orders_on_close` | both fill at the close; the book is called at the next open, sized at that close | `7493ebfddfb2e12d79f768ca2ab1362b30fc71a6f805000841238a10c6084b85` |
| `w5b-sb-default-x3` | 48 | `pyramiding=3` | three default (100 %) entries on one bar fill at the next open and are called whole there; where that open gaps over the signal close, all three are refused | `e3b0412226c3233aefdf58b099f3c3861faf85fcfddc84a6215b1ce5dd653ab5` |
| `w5b-sb-pooc-default-x3` | 60 | the same, `process_orders_on_close` | all three fill at the close and are called whole at the next open | `0a8635a1bb88f917c614994772c7d40575c6ea1f5631e60e398828aa17870394` |
| `w5b-sb-market-40x2` | 80 | `pyramiding=2` | the control: two 40 % entries, an 80 % book, fill with no call | `7871b625e8386a6497860bcb5989132257bfad11a5a6cfd6485cca3d8e489264` |

The adapter judged each later entry at its fill on the book the earlier ones
had opened, refused it, and never called the book: it filled the first entry
of each cell and nothing else. TradingView judges each entry on its own cost,
as it judged the first, and checks the combined book once the last has filled
(the CG-LINUX-RED `env_facts` export of 2026-09-26 is the same shape).
