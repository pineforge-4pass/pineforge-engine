# Which order fills first at one fill point (lane W6-ENG-FILL-ORDER)

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered) of a synthetic probe written for this lane, byte for
byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. `metrics.json` `tvTradesCsvHash` is the sha256 of
`tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15, 2025-04-01 ..
2025-05-01, with fixed `timestamp("UTC", ...)` cells on 2025-04-07 ..
2025-04-10 and a cleanup (`strategy.cancel_all()` plus `strategy.close_all()`)
after each cell. No closed or scraped strategy is involved.

`tests/test_same_point_entries_tapes.cpp` replays the tapes through the Pine
adapter under the configuration the generated constructor declares for each
probe, over the corpus 15m bars of `../tvdef_drops/bars.inc`, trading from the
bar before TradingView's first entry as `run_strategy.py` does for a corpus
tape, with TradingView's 0.0001 lot as the `qty_step`, and requires each trade
the tape closes in the row's cells -- entry and exit time, side, price in
ticks of 0.01, quantity in lots of 0.0001 -- to be the engine's. It also reads
the order off TradingView's own rows on every pair tape, the ones it does not
replay included.

## F10: a flat pair of entries at one open

| tape | trades | `strategy()` declares | tv_trades.csv sha256 |
|---|---:|---|---|
| `w6-f10a-open-pair` | 40 | fixed 1, pyramiding 0, capital 1000000 | `b93840d8a6a8f35a379d975017b1a5ff90366ab672764da4339bde0dfc981295` |
| `w6-f10b-limit-class` | 24 | fixed 1, pyramiding 0, capital 1000000 | `2a3f906b05037f148f4b3be8d53a2267a5c2031c29acc86400f249b0a54228a6` |
| `w6-f10c-same-side-class` | 32 | fixed 1, pyramiding 2, capital 1000000 | `7bac616e77cbe1817679ad796fd18a8592b4d79abbafd9f7900333484b546b0e` |

Every two hours from 2025-04-08 00:00 UTC, while flat, a probe places two
`strategy.entry` calls on one bar, each a market order or a stop or limit
already through the price (a buy stop or sell limit at half the close, a sell
stop or buy limit at twice it), so both fill at the next open. A cell is named
by the two kinds (M market, S stop, L limit) and by the side declared first
(LF long first, SF short first); `-Q` cells trade 2 and 3 units. Each cell
runs twice.

TradingView fills the calls that reach one fill point in a fixed order of
side and kind:

| rank | 0 | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|---|
| order | buy market | buy stop | sell market | sell stop | buy limit | sell limit |

and one rank in the order the calls were placed. So a buy stop placed after a
sell market fills first (`MS-SF`), a sell market fills before a buy limit
placed after it (`ML-SF`), and a sell market placed after a buy limit fills
first (`LM-LF`); with pyramiding 2, a market lot fills before a stop or limit
lot of the same side placed before it (`w6-f10c`).

The later of two opposite calls is one transaction of its own quantity plus
the earlier pending MARKET call's (a pending stop or limit lends it nothing),
and the earlier call trades only its own. A buy that fills first therefore
opens both quantities as one lot and the sell closes part of it: the tape's
two rows carrying the buy's signal (`MS-SF`: long 1 closed by `MS-SF-1`, long
1 closed by the cleanup; `MS-SF-Q`: long 2 and long 3 of a buy of 5).

The engine queued the calls in the order they were placed; the post-evaluation
pass `PineExecutionAdapter::order_same_point_entries` now queues this bar's
marketable flat explicit-quantity entries in TradingView's order (a keep-handle
re-price), and a marketable buy stop placed after a sell market is one
`Transact` of both quantities.

## F10: the later call is costed with the pending market

| tape | trades | `strategy()` declares | tv_trades.csv sha256 |
|---|---:|---|---|
| `w6-f10g-pair-gross` | 38 | fixed 1, pyramiding 0, capital 3000 | `7af593068f16e6259412bf818a2d5b5bea715940c3dd362735a5687a8ab16d06` |
| `w6-f10h-pair-gross-pyramiding1` | 38 | fixed 1, pyramiding 1, capital 3000 | `7af593068f16e6259412bf818a2d5b5bea715940c3dd362735a5687a8ab16d06` |

One-contract pairs (`MM-LF`, `MM-SF`, `MS-LF`, `MS-SF`, seven passes) on 3000
of equity: each call alone costs about half of it, the later call's
transaction (its own contract plus the pending market's) costs more than the
equity whenever the close is above about 1500. TradingView keeps the later
call exactly when that transaction, priced at the signal close, is within the
equity; past it only the earlier call trades. Pyramiding 1 is the same tape.
The engine had costed the later call's own contract only.

## Cells TradingView and the engine still book differently

The test leaves these out; the tapes stay as evidence.

- `w6-f10c-same-side-class`, pyramiding 2: every cell. The engine keeps
  placement order between two same-side lots at one open; TradingView fills
  the market lot, then the stop, then the limit (the tape reading holds it).

## Bars

The replayed bars are `../tvdef_drops/bars.inc` (2025-04-07 00:00 ..
2025-04-10 12:00 UTC, the corpus 15m chart feed, sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived
from the corpus 1m feed, sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`).
