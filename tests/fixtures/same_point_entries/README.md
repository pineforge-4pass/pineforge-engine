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

## F10: a flat market pair at one close

| tape | trades | `strategy()` declares | tv_trades.csv sha256 |
|---|---:|---|---|
| `w6-f10d-pooc-pair` | 26 | fixed 1, pyramiding 0, process_orders_on_close | `9f87d773f99b96271e4909104825a221aed8b7edaeed841e5b4cee8322238337` |
| `w6-f10e-pooc-coof-pair` | 26 | the same and calc_on_order_fills | `9f87d773f99b96271e4909104825a221aed8b7edaeed841e5b4cee8322238337` |
| `w6-f10f-coof-pair` | 26 | fixed 1, pyramiding 0, calc_on_order_fills | `f4a5912c8a6af97c06fa918b95ed4aa3afc4d43543103fa39ae415f845599ad1` |

The same order holds at a process_orders_on_close close, with or without
calc_on_order_fills (the two tapes are one file), and at the next open under
calc_on_order_fills alone. The adapter's close pass for a flat explicit
market pair (`apply_terminal_explicit_market_policy`) had kept source order
with a reversing second call; a sell-then-buy pair now fills the buy first as
one transaction of both quantities.

## F12: a reversing stop and a protective stop on one bar

| tape | trades | `strategy()` declares | tv_trades.csv sha256 |
|---|---:|---|---|
| `w6-f12a-stop-priority` | 17 | fixed 1, capital 1000000 | `c4da60fa033b174a545878f01b5e4f72c07003dc4237879c235b8461572c4c6f` |
| `w6-f12b-stop-priority-default` | 11 | nothing (v6: percent_of_equity, 100) | `8f80409878ba8a5b5aca2683e1cacb476a00576557a4ff41677d9241a96cabc4` |
| `w6-f12c-stop-priority-magnifier` | 17 | fixed 1, capital 1000000, use_bar_magnifier | `ac013fa7cffeba59ba0d93575539c4ce407510a951eda6987faec9ec440608dc` |

A held position carries a protective `strategy.exit` stop X while an opposite
`strategy.entry` stop Y rests on the same side of the price, and one bar
reaches both. A1/A2 (long, Y above X, a falling bar whose open is nearer the
high / the low) and B1 (the short mirror): the reversal fills at Y first and
the exit is void. A3 (X above Y): the exit fills at X, then the entry opens at
Y from flat with its frozen quantity (2: its own plus the position it was
placed against). A4/A5/B2 (both already through when placed, so both reach
the next open; entry placed first / exit placed first): the exit fills first
at the open, whichever was placed first, and the entry then trades its frozen
quantity from flat. A6 (only Y through): the reversal at the open. A7 (only X
through): the exit at the open. The magnifier tape books the same trades
(only the excursion columns differ). The engine books every fixed-size cell.

Under the v6 default size (`w6-f12b`), A1/A2 keep only the reversal's closing
leg (lane TVDEF-DROPS R1) and A3's entry cannot fund its frozen quantity after
the exit. In B2 the short was partly margin-called on its entry bar, its
reversal is declined at the open, and TradingView still fills the protective
stop there. `PineExecutionAdapter::defer_declined_reversal_exits_at_adverse`
had cancelled that stop (or moved it to the bar's adverse extreme); a stop the
open is already through now fills at that open.

## Cells TradingView and the engine still book differently

The test leaves these out; the tapes stay as evidence.

- `w6-f10c-same-side-class`, pyramiding 2: every cell. The engine keeps
  placement order between two same-side lots at one open; TradingView fills
  the market lot, then the stop, then the limit (the tape reading holds it).
- `w6-f10d-pooc-pair` / `w6-f10e-pooc-coof-pair`: the cells with a priced leg
  (`MS-SF`, `SM-SF`, `LM-SF`, `LM-LF`, `ML-SF`). Under
  process_orders_on_close a priced leg placed at the close fills at that close
  through the adapter's POOC close pass, which neither orders it against the
  market leg nor adds the pending market's quantity.
- `w6-f10f-coof-pair`, calc_on_order_fills without process_orders_on_close:
  every cell. The engine neither orders the pair nor adds the pending market's
  quantity outside `same_bar_market_tx_scope()`.
- `w6-f12b-stop-priority-default` A4: after the exit, TradingView opens the
  reversal's frozen 2x short at the open and liquidates it at once by a margin
  call at the same price; the engine refuses the fill. A margin-model rule,
  not an ordering one.

## Bars

The replayed bars are `../tvdef_drops/bars.inc` (2025-04-07 00:00 ..
2025-04-10 12:00 UTC, the corpus 15m chart feed, sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived
from the corpus 1m feed, sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`).
