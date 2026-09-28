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
bars, 2025-04-01 .. 2025-05-06, every NYSE:F probe on 15-minute bars,
2025-04-15 .. 2025-05-20, and every OANDA:EURUSD probe on 15-minute bars,
2025-04-01 .. 2025-04-30 (`w5b-sz-eur-p100-10m-0410` and the `w5b-ms-*` probes
.. 2025-04-10), and
every CME_MINI:ES1! probe on 15-minute bars, 2025-04-01 .. 2025-04-30. The
probes of rules SB, PC and PA declare no `initial_capital` and run on v6's
100000; SZ's, FU's and MS's declare theirs.

`tests/test_margin_residual_tapes.cpp` replays every tape through the Pine
adapter under the configuration the generated constructor declares for that
probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
(`tests/fixtures/margin_v6/bars.inc`, 2025-04-01 00:00 .. 2025-04-17 00:00
UTC), the NYSE:F 15m bars in `f15_bars.inc` or the OANDA:EURUSD 15m bars in
`eur15_bars.inc`, with TradingView's lot (0.0001 ETH, one NYSE:F share, 0.01
EUR) as the `qty_step`, and requires each trade the tape closes inside the
replayed bars (or before its own window's end) -- entry and exit time, side,
price in ticks (0.01; 0.00001 on OANDA:EURUSD, 0.25 on CME_MINI:ES1!, whose
contract is the lot), quantity in lots -- to be the engine's. On the commit before each rule's (the lane's base 1ed0a9e4 for SB,
PC and PA) every rule tape below fails there and every control passes.

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

## SZ: a percentage of equity rounds the order's money, not the equity

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-sz-eur-p50-10m` | 504 | `initial_capital=10000000`, 50 %, `process_orders_on_close` | every 4 bars from flat a default long filled at the close, flattened two bars later: each quantity is the lot floor of the order's money, 50 % of the equity, rounded to ten significant digits | `6f2cb986c35b2630ee15a74d717e442f9e7248acf462c745f62fedd3442f9cc3` |
| `w5b-sz-eur-p33-10m` | 504 | the same at 33 % | the same | `9c51410478c7980553752bf185d59a8a11d57f403c4862cbc3c35a6f53426569` |
| `w5b-sz-eur-p100-10m-0410` | 130 | the same at 100 %, to 2025-04-10 | the control: at 100 % the money is the equity, so both roundings are one number | `333d87b5e7d84396e7804d267536c583f4d5391cc0a754d51b9985f32e10f9ff` |

On ten million the equity carries more decimals than ten significant digits
keep. The adapter rounded the equity to ten digits before taking the
percentage, which floors 44 of the 504 quantities at 50 % and 37 at 33 % one
lot away from TradingView's (no rounding at all misses 8 and 18, and 98 of
the 130 at 100 %). The population probe `margin-basis-frac` on OANDA:EURUSD
(0.01 lots, 50 %) is the same shape: replayed from TradingView's first bar,
every one of its 3352 closed quantities matches under the order's money
rounded; under the equity rounded 1327 are a lot off and one add is dropped.

## FU: a whole-contract book's call is followed by one contract

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-sb-es-fixed-n` | 469 | `initial_capital=1000000`, `strategy.fixed` 1, `pyramiding=20` | CME_MINI:ES1! (point value 50); every 2 hours from 2025-04-01 14:00 UTC, 120 cells of 3 + cell % 5 one-contract MARKET longs from flat, flattened an hour later: a book 4 x the floored restore leaves is called one contract more at the bar's next path point after the open (5641.5 open, 4 called there, 1 at the 5642.5 high); a sub-lot restore (one contract) or a call of the whole book takes none | `303f1a853ac6dbeef1bb5ec4c9bd6d3614b267c5e6e548b81fa33e04032b5d3d` |
| `w5b-sb-es-pooc-fixed-n` | 469 | the same, `process_orders_on_close` | the follow-up contract at the same next open as the call | `cff469f01599a3f7b0558040c59772acc63f18e1e449f881234bbdeaa8b7a069` |
| `w5b-es-short-q111111` | 528 | `initial_capital=1300000`, six one-contract shorts | the same for a short (4 at the open, 1 at the next path point) | `9233b1b988d36971f40d51567835d782932b6efb9c37cab477fdd4b923a6c344` |
| `w5b-es-pooc-short-q111111` | 528 | the same, `process_orders_on_close` | 4 and 1 at the next open | `ac37db48e2e1d9fcc1acd46c84627f0640a588d4b910b58fadaea6ffd284c9dc` |
| `w5b-es-q1122` | 435 | entries of 1, 1, 2, 2 contracts | the oldest lot one contract: 4 then 1 | `3a238dda23710a935c2925ef468ffb381ed1b08d30e57042db99e002746d693d` |
| `w5b-es-pooc-q1122` | 436 | the same, `process_orders_on_close` | 4 then 1 at the next open | `1c6d66381cfab45386c86e35a64e3b21fa8591fc8dec028cccd34501067e383a` |
| `w5b-es-q115` | 1051 | `initial_capital=1600000`, entries of 1, 1, 5 | the call ends inside the third lot: still 4 then 1 | `cd5a3019b5312e2a0101c7187ec5fc8926a1af13700ce635498cc337bb4f2b99` |
| `w5b-f-q1x12` | 833 | NYSE:F, `initial_capital=100`, twelve one-share entries from 09:30 New York | 5 of 12 called in every one of the 70 cells: 4 at the open, 1 at the next path point | `fe3249d640ad74a93f361d210daa9bfec142cce54512c37be638eea6e32cebb7` |
| `w5b-es-q21111` | 445 | entries of 2, 1, 1, 1, 1 | the control: the oldest lot two contracts, 4 called and no follow-up | `61b572f2c010afd8d4f4409373bc3574e8e6f12d5ad37f6238794a847183eb7c` |
| `w5b-es-pooc-q21111` | 445 | the same, `process_orders_on_close` | the control, at the next open | `634b37eef504347b8f0ea60e9256173b16824a47d3fbc7bb3a5a4bc636daab77` |
| `w5b-es-q222` | 269 | entries of 2, 2, 2 | the control: 4 called, none more | `2c5ef872cbf236213e5b4d38518f22d88d58c37de7dbca7d85ccc55fd9875eb1` |
| `w5b-eth-q111` | 480 | BINANCE:ETHUSDT.P, `initial_capital=5000`, three 1 ETH entries | the control on a 0.0001 lot: 1.1848 called, none more | `f8acd4bcb9bade3c8048e1e49a074a872bc60d736f3b9207165f1c642cedc5f4` |

TradingView calls four times the floored restore (4 x floor(shortfall / unit
margin) contracts) where the adapter did, and then one more contract at its
next check point when the book's oldest lot was one contract on a whole-unit
grid; the adapter took the first call only. The oldest lot decides it, not
where the call ends: 2, 1, 1, 1, 1 takes no follow-up, 1, 1, 2, 2 and 1, 1, 5
do. The same-bar probes of rule SB are the shape (the ES callsite probes of the
population, five one-contract process_orders_on_close entries, are called 4 +
1 at the next open).

Two of the scripts (`w5b-es-q115`, `w5b-f-q1x12`, n = 400) put `n * step`
past 2^31, which TradingView evaluates in 64 bits; the replay's probes keep
their time in 64 bits too.

## MS: the one-unit money call does not read `pyramiding`

| tape | trades | `strategy()` declares | TradingView | tv_trades.csv sha256 |
|---|---:|---|---|---|
| `w5b-ms-eur-p100-pyr2` | 130 | `initial_capital=10000000`, 100 %, `pyramiding=2`, `process_orders_on_close`, to 2025-04-10 | the `w5b-sz-eur-p100-10m-0410` cells (one lot held at a time) under `pyramiding=2`: the same file, fourteen one-unit money calls included | `333d87b5e7d84396e7804d267536c583f4d5391cc0a754d51b9985f32e10f9ff` |
| `w5b-ms-eur-p99-pyr2` | 168 | the same at 99 % | the control: no margin call at all | `bed349e5aa1487233d69cb83e111d145b6b5f2202de516bddbd5d13c082b625c` |

A full-margin long of one lot under process_orders_on_close takes
TradingView's one-unit money call (the ten-significant-digit residual) at
pyramiding 1; ab9714be scoped the call to pyramiding 1 and the adapter
inherited it, so at pyramiding 2 it booked none of the fourteen. The
population probe `margin-basis-allin` on OANDA:EURUSD (100 %, `pyramiding=2`)
is the same shape (`tests/test_tv_money_carried_pooc_l4b.cpp`'s pyramiding
scope moves with it).

## Bars

`f15_bars.inc` is the NYSE:F 15m chart feed of the lab lane f-15 (evidence
sha256 `80f404ae85ef0b6a0d8056a90997e92fa1236f1ae68b75c1b2d3a6f182558e32`),
rows 2025-04-15 13:30 .. 2025-05-19 19:45 UTC copied as text; the lab tv
window ends before 2025-05-20's session. `eur15_bars.inc` is the OANDA:EURUSD
15m chart feed of the lab lane eurusd-15 (evidence sha256
`f945f31a140aedc64ca24e2ace42b9885a914ce31a2531f74179b703c025a648`), rows
2025-04-01 00:00 .. 2025-04-29 23:45 UTC copied as text, the first of them
TradingView's bar_index 0. `es15_bars.inc` is the CME_MINI:ES1! 15m chart
feed of the lab lane es1-15 (evidence sha256
`766e8149d7e16be49cd030f0993eeb339199e0fd4a97d7603625e6a3fe583471`), rows
2025-04-01 00:00 .. 2025-04-29 23:45 UTC copied as text.
