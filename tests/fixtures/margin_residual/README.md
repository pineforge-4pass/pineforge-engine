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
sha256 of `tv_trades.csv`. Every BINANCE:ETHUSDT.P probe runs on 15-minute
bars, 2025-04-01 .. 2025-05-06, and every NYSE:F probe on 15-minute bars,
2025-04-15 .. 2025-05-20; none declares `initial_capital`, so each runs on
v6's 100000.

`tests/test_margin_residual_tapes.cpp` replays every tape through the Pine
adapter under the configuration the generated constructor declares for that
probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
(`tests/fixtures/margin_v6/bars.inc`, 2025-04-01 00:00 .. 2025-04-17 00:00
UTC) or the NYSE:F 15m bars in `f15_bars.inc`, with TradingView's lot (0.0001
ETH, one NYSE:F share) as the `qty_step`, and requires each trade the tape
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

## PC: a carried process_orders_on_close short is checked before the script

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-pc-seen-size` | 36 | `process_orders_on_close` | fifteen default shorts filled at a signal close; on the next bar, whose high puts the short under its margin, the script buys back `math.abs(strategy.position_size)` with `strategy.order`: the call is taken at the high first and the script reads the called size, so the buy closes exactly the rest | `ee4b13e93ece389691a84d657851cfd796f8c9319a842eb8ce67346e390b80fb` |
| `w5b-pc-seen-size-parked` | 36 | the same | the same with a far long limit entry parked beside each short: the same file | `ee4b13e93ece389691a84d657851cfd796f8c9319a842eb8ce67346e390b80fb` |
| `w5b-pc-carried-close` | 36 | the same | `strategy.close_all` at the next close: the call at the high, then the close of the rest | `7f3f83734958534c3457d5b7db39af145a8f0f60511352dc5befafdb7477fd2a` |
| `w5b-pc-carried-reverse` | 51 | the same | a default long at the next close: the call at the high, then the reversal sized from the called book's equity | `e7335b02f293337f26ca60468a3c8255361f2974a72f08ee53857342b52afd2d` |
| `w5b-pc-bracket-tp` | 35 | the same | each short carries `strategy.exit(limit = close * 0.998, stop = close * 1.03)`: on a bar that first rises over the fill and then falls to the take-profit, the call at the high comes first and the take-profit fills the rest | `5c5afc0bc65fd4df5158429899fe045bff9d262cbe0fc682778d8fa72a2e860d` |
| `w5b-pa-pooc-p50` | 109 | `process_orders_on_close`, `pyramiding=2`, 50 % | E1 then an add E2 45 minutes later: a short add filled at a close is not checked at that bar's high on the grown book; the grown book is called at the next open | `79a5c58b0ef2b2109d16bf5cea9b041d224d62fd5ddd7f62b4ed2178daec9305` |
| `w5b-pc-carried-none` | 37 | `process_orders_on_close` | the control: nothing at the next close but a `strategy.cancel_all()` | `415df80fea9981cd4370397cd527173bdff984a22b811435c7fa0b265c49855d` |

ab9714be took no path check on such a short beside a competing pending entry
or on a rounded-money one, and checked it at the high after the script
instead, deferred behind the close's market fills to the book they left: the
script read the uncalled size, a close or bracket that emptied the book on
the path left no call, and an add at the close was called at the high on the
grown book. Lane W5-ENG-MARGIN-V6 moved the commissioned and slipped shorts
before the script (C1); these tapes show the rest.

## PA: an add is judged with no lot of slack

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-pa-f-pooc-p50` | 248 | `process_orders_on_close`, `pyramiding=2`, 50 % | NYSE:F, a long E1 at 09:30 .. 14:30 New York and an add E2 at the next close: every add whose margin on top of the held long's exceeds the equity at the close's tick is dropped, by less than one share's notional ($1.90 .. $9.95) as by more | `e9100da27997e4a07510a854434f63899f52ab0f794bc88d29ca91e06fe60be6` |
| `w5b-pa-f-market-p50` | 245 | `pyramiding=2`, 50 % | the market twin: E1 fills at the next open, E2 is placed at that bar's close; the same judgement at the signal close | `615ca02b3260e0c588ba9f7dcce0e912678d297565a8ea578bdf5cc773f9b992` |
| `w5b-pa-pooc-p40` | 120 | `process_orders_on_close`, `pyramiding=2`, 40 % | the control: 40 % + 40 % fits the equity, every add fills | `03b46092a38cac2d88db3427203b6b02b45fd014af0f0dd336a06bd093dfb584` |
| `w5b-pa-market-p50` | 109 | `pyramiding=2`, 50 % | the control on BINANCE:ETHUSDT.P's 0.0001 lots, market | `a3e688d70b247d351030a14678f5c457e649623f0c60b7764c8b8a58ef89d99f` |
| `w5b-pa-pooc-p50-tick` | 171 | `process_orders_on_close`, `pyramiding=2`, 50 % | the control: an add only at a close within two ticks of E1's price | `f2586b6b88278880dbb42d275c0922c63533186351a4553f2e837f2e233d675d` |

The adapter admitted an add whose shortfall stayed within one lot's notional
(the market one at placement, W5-ENG-MARGIN-V6's M1; the process_orders_on_close
one at its close fill) and then called one whole unit of the book; TradingView
drops it (`tests/test_reversal_admission_float_guard_l4c.cpp` pin E moves with
it).

## Bars

`f15_bars.inc` is the NYSE:F 15m chart feed of the lab lane f-15 (evidence
sha256 `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`),
rows 2025-04-15 13:30 .. 2025-05-19 19:45 UTC copied as text; the lab tv
window ends before 2025-05-20's session.
