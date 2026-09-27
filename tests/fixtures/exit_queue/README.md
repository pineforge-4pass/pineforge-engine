# strategy.exit reserves an entry's quantity in creation order (lane W3-ENG-EXIT-ALLOC)

TradingView reserves an entry's quantity for its `strategy.exit` orders in the
order the exits were created. Re-issuing an exit modifies it in place and keeps
its place in that queue; a later-created exit gets only the quantity the
earlier ones leave. Of two exits without `qty` on one entry, only the one
created first ever fills. Population family F05 (TAIL-TRIAGE): a script that
issues a stop and then a target for its entry on every bar shows only stop
exits on TradingView's tapes, and the engine filled the targets.

The adapter refused a new exit its elders had fully reserved, but a re-issued
exit counted every other live exit against itself, the ones created after it
included. Exits a script issues every bar while flat stay live and
unreserved; on the bar an explicit-quantity entry is placed under
`process_orders_on_close`, the pending entry gives them a quantity, and the
first exit re-issued there lost the whole position to the later one.

Each directory is one `lab tv` export (channel `ws-report-v1`, `rangeProof`
covered, `--no-note`), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`. Every probe runs on BINANCE:ETHUSDT.P 15,
2025-04-01 .. 2025-05-01 and omits `initial_capital` (v6: 100000). Except
where a table says otherwise it declares `default_qty_type = strategy.fixed,
default_qty_value = 1`, enters one long at 00:00 UTC on 2025-04-08, 09, 10, 11
and 14, and ends each with a cleanup `close_all` six hours later. Exit A is a
stop and exit B a limit; their levels are set from the entry bar's close
(`ref`).

`tests/test_exit_queue_tapes.cpp` replays every tape through the Pine adapter
under the configuration the generated constructor declares for that probe,
trading from the first cell (2025-04-08 00:00 UTC) with TradingView's 0.0001
lot as the `qty_step`, and requires each trade the tape closes inside the
replayed bars -- entry and exit time, side, price in ticks of 0.01, quantity
in lots of 0.0001 -- to be the engine's. It also reads the rule off
TradingView's own rows.

## The rule tapes (the engine filled the later exit before the fix)

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3f05-s5-pooc-qty-stop-then-target` | `process_orders_on_close`, entry `qty=1`; every bar from the first entry, flat or not, stop A (`ref - 40`) then target B (`ref + 4`): only A fills, although B's level is reached first in four of the five cells | `744eb973818d1ae82e8b1ad7b5ed02bb18fcd21abf5af5a72d34020d12adaf57` |
| `w3f05-s6-pooc-qty-target-then-stop` | the same exits issued B first: only B fills; where the market reaches A first (04-09), A never fills | `308888fa365b7facde437f6f5123df8ccd2f89feaf1a6368c61681f734d44cc8` |
| `w3f05-s8-pooc-qty-half-then-full` | A for 50%, then B without qty: B gets the other half | `98dbc59b3ffbeaa33f8d3604a095a83909c3a178cab4cf9281858a1c4fbe3859` |
| `w3f05-s8b-pooc-qty-full-then-half` | A without qty, then B for 50%: B gets nothing (the rows are s5's) | `744eb973818d1ae82e8b1ad7b5ed02bb18fcd21abf5af5a72d34020d12adaf57` |
| `w3f05-s13-pooc-qty-shared-oca` | s5 with both exits in `oca_name = "X"`: the OCA name changes nothing (s5's rows) | `744eb973818d1ae82e8b1ad7b5ed02bb18fcd21abf5af5a72d34020d12adaf57` |

## The controls (the engine already booked them)

| tape | TradingView | tv_trades.csv sha256 |
|---|---|---|
| `w3f05-s1-stop-then-target` | s5 without `process_orders_on_close`: only A fills | `ceec4a6dabf04df9ae3c04fe972611219d6904aadbc8b8eb72fe35826eb38b01` |
| `w3f05-s2-target-then-stop` | s6 without `process_orders_on_close`: only B fills | `825cb56c60a945465fdae39aa3222822d6510840a2ca8f0ae780803109dfdbd6` |
| `w3f05-s7-pooc-qty-half-each` | A and B for 50% each: each fills its half (s8's rows) | `98dbc59b3ffbeaa33f8d3604a095a83909c3a178cab4cf9281858a1c4fbe3859` |
| `w3f05-s10-pooc-qty-target-created-first` | B (`ref + 40`) alone on the entry bar, then, while the long is held, a near stop A (`ref - 8`) first and B again: B, created first, keeps the position; A never fills | `b9dc8f33383aab4d8c9033ccde1f49c8b76a892ad7c2c4031e897d8b0c8a48ae` |
| `w3f05-s14-pooc-qty-stop-once` | A issued once on the entry bar, B on every bar held: only A fills (s5's rows) | `744eb973818d1ae82e8b1ad7b5ed02bb18fcd21abf5af5a72d34020d12adaf57` |
| `w3f05-s15-pooc-qty-cancel-then-redeclare` | A cancelled an hour after the entry and issued again after two with a marketable stop (`close + 50`), behind B (`ref + 100`, never reached): the new A never fills | `f59409eb0f0ba90ba2509c96340435a35bde9857641eb8566b940d4173c1cf5a` |
| `w3f05-s16-pooc-qty-first-ever-stop` | the first trade issues only A; later trades issue B alone on the entry bar, then A first and B: A's first trade gives it no place in later ones | `d21cbeacd02510906b27e850eac1631b1bc901a25c06eb41e3ce45ff43873503` |
| `w3f05-s18-pooc-qty-exit-before-entry-call` | B issued once on the entry bar, before the `strategy.entry` call: void | `8396c7e265cd69e1549265167cc84df57fe1391c37ed4c20631d18aa5ace0646` |
| `w3f05-s21-exit-before-entry-call-nextopen` | s18 without `process_orders_on_close`: void | `56b1bb4e8d41a48253d2c798ab878fcce5d2441cb7a16f2f9afede101675b508` |
| `w3f05-s20-exit-for-pending-limit-entry` | a limit entry 15 below the close; B (the limit + 20) issued once on the next bar while the entry rests: B is the entry's when it fills | `6b29fea54cb7b2293dfcb9c83883363c5cb1aeced3b8ffb1ac906bf825979f2f` |

## Bars

`bars.inc` holds the replayed bars, 2025-04-07 00:00 .. 2025-04-14 12:00 UTC,
copied as text from the corpus 15m chart feed `scripts/derive_corpus_feeds.py`
derives (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`) from the
corpus 1m feed (sha256
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`);
`exec/W3-ENG-EXIT-ALLOC-scratch/tools/gen_bars_inc.py` wrote it.
