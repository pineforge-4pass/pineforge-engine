# Several adds one bar places on a held position (lane TAIL-B)

A grid bot buys every level the close crossed, so one bar can place many
MARKET entries on top of a position it already holds. TradingView's broker
treats such a bar's adds as a group, and three rules follow from its tapes.
Each is pinned here by synthetic probes written for this lane; no closed or
scraped strategy is involved.

- **AS (admission).** Each add is judged against the position held when it was
  placed plus its own units, not against a sibling that filled a moment earlier
  at the same point. Every add that fits beside the held position alone fills,
  under `process_orders_on_close` at the close as at the next open (rule COQ,
  `tests/fixtures/coqueued_open_margin`, covered only the next open).
- **AC (one call).** The grown book is margin-called once, after the last add
  has filled: four times its lot-floored shortfall, not a call after each add.
  A market add's call is taken at that fill -- a commissioned explicit short's
  too, at the print its fill slipped from. Under `process_orders_on_close` the
  call is taken at the next open, sized at the close the adds filled at, long
  or short, fees or none.
- **LF (the follow-up).** After a call that closes several lots of a long,
  TradingView's broker checks the book again after each lot, first in first
  out, on a state that has taken the lot out of the book but not yet booked its
  P&L: that lot relieves the shortfall by its cost (its entry fee was booked
  when it opened), each lot before it, booked, by what its sale fetched net of
  its exit fee. The last check that is still short is called again, four times
  its lot-floored shortfall: at the same open under `process_orders_on_close`,
  at the bar's next path point after a market call. A call that starts inside
  a lot costing more than the shortfall is not followed.

Every probe runs on BINANCE:ETHUSDT.P 15-minute bars, 2025-04-01 .. 2025-05-01,
with 10000 of capital, `pyramiding = 200` and margins of 100. Every quantity is
money / close floored to 0.0001, never `strategy.equity`: under a commission
TradingView's `strategy.equity` is net of the open trades' entry fees, which the
engine's is not, and an equity-sized order would differ by that alone.

Each directory is one `lab tv --no-note` export (channel `ws-report-v1`,
`rangeProof` covered), byte for byte: `strategy.pine`, `tv_trades.csv` (times
at UTC+8), `metrics.json`, `meta.json`. `metrics.json` `tvTradesCsvHash` is the
sha256 of `tv_trades.csv`.

## The cycle probes (rules AS and AC)

Every 3 hours from 2025-04-02 00:00 UTC, forty cells: from flat a seed of 9000
(every fifth cell two seeds of 4500, a bar apart); two bars later 1, 2, 3 or 5
adds of 700 each (the fifth kind: 3), placed on one bar; flattened 75 minutes
into the cell. One add fits beside the seed; two or more together do not.

| tape | `strategy()` declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `tailb-adds-pooc-c6s3` | long, `process_orders_on_close`, commission 0.06 %, slippage 3 | 109 | `bae8f69537839818ba92e2d50e40d424c5a4b927a679a7f9332fdc26fc89596b` |
| `tailb-adds-pooc-c10s3` | long, `process_orders_on_close`, commission 0.1 %, slippage 3 | 97 | `e59baddab49ba40f94763796411a1a31c3f0a4a57f38990e7e61db66b9faf59a` |
| `tailb-adds-pooc-c0s0` | long, `process_orders_on_close`, no fees | 152 | `ac675d467a073cea705e826c956f57ece37d28a54ffbb6079e99fba0fe87367d` |
| `tailb-adds-pooc-c6s3-short` | short, `process_orders_on_close`, commission 0.06 %, slippage 3 | 195 | `e837d67a02aa785b3dc265deca25dcc2a6f2df65509f6f1624c447c57cadee93` |
| `tailb-adds-mkt-c6s3` | long, commission 0.06 %, slippage 3 | 109 | `4cc2601f544ccc8bd0d5bd1cdb7ae806e0ed2d8c8c1db0359163e1c37f51cd5f` |
| `tailb-adds-mkt-c10s2` | long, commission 0.1 %, slippage 2 | 97 | `2f77209860e41adff58d852c9e0d66e468f3ff6e2700966cba2c8a9594a29c67` |
| `tailb-adds-mkt-c0s0` | long, no fees | 152 | `d508c69bc7ff6f740a496bf3330bc4f758430438ed9d7707e1684f1666607096` |
| `tailb-adds-mkt-c6s3-short` | short, commission 0.06 %, slippage 3 | 195 | `7ecd1ec485190a227c794c86f0830983edc51bb54a521866306eb8b3f97b050d` |

Wherever the adds together exceed the equity, every add that fits beside the
held seed fills and one call takes four times the whole book's lot-floored
shortfall -- at the adds' own open for a market run, at the next open under
`process_orders_on_close`, sized at the close. Once losses bring the equity
under a seed and an add's cost, the later cells' adds are refused (a control
of the per-add admission) and those cells keep their seed alone.

## The loss probes (rules AS and AC)

Fixed cells: a seed of 9000 on 2025-04-06 06:00, three adds on 04-07 06:00
after a 15 % fall; a seed on 04-08 12:00, five adds on 04-09 06:00; four seeds
of 2250 on 04-10 00:00 .. 00:45, three adds at 18:00; a seed on 04-11 06:00,
three adds at 06:30. Each flattened after its adds.

| tape | `strategy()` declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `tailb-loss-pooc-c6s3` | long, `process_orders_on_close`, commission 0.06 %, slippage 3 | 20 | `885fada2be11a5b810125c5b6c24d000a127b295c4d1f5b3ce7933d98aa00dbc` |
| `tailb-loss-mkt-c10s2` | long, commission 0.1 %, slippage 2 | 20 | `d48e337582654c9ffa5696e643f54c8860db9504cebd66a5d01059a881d3f19b` |

The four-seed cell's call takes the first seed whole and part of the second:
that seed costs more than the shortfall, so no follow-up.

## The lots probes (rule LF)

A book of N seeds of 9000 / N each, one per bar from 2025-04-06 00:00 UTC,
then three adds of 700 on the 2025-04-07 06:00 bar; flattened at 08:00.

| tape | `strategy()` declares | trades | tv_trades.csv sha256 |
|---|---|---:|---|
| `tailb-lots12-pooc-c0s0` | 12 seeds, long, `process_orders_on_close`, no fees | 17 | `f2a3285447ca9a9618783e170dc9f2fbca5eade3290de015e0caa0672a78c80a` |
| `tailb-lots30-pooc-c10s3` | 30 seeds, long, `process_orders_on_close`, commission 0.1 %, slippage 3 | 35 | `1af2f5d2d105f782179603b771083489e6021712ef473f64fb276dc3c9bbdef1` |
| `tailb-lots30-mkt-c10s2` | 30 seeds, long, commission 0.1 %, slippage 2 | 35 | `3db6e37fd08c5c5f10e3072db87411db94416786c4ac397d6ca275a6dc64357a` |

Each call closes the oldest seeds whole and part of the next, and a second
call follows: at 06:15's open (1539.97, both calls) under
`process_orders_on_close`, at 06:15's high (1541.74) after the market call at
its open. The same rule gives every call on thirteen further lots probes'
tapes (scratch exports, not kept): 12, 16, 20, 25, 30, 40 and 50 seeds without
fees; 10, 30 and 60 seeds at commission 0.1 % and slippage 3 (the 10-seed
book, whose oldest seed costs more than the shortfall, takes no follow-up); 30
seeds at each alone; and the 30-seed probe's market twin. It gives the
3Commas grid bots' call pairs on BINANCE:BTCUSDT and BINANCE:ETHUSDT.P daily
bars too.

## The replay

`tests/test_add_sibling_margin_tapes.cpp` replays every tape through the Pine
adapter under the configuration the generated constructor declares for its
probe, over the corpus 15m bars of lane W5-ENG-MARGIN-V6
(`tests/fixtures/margin_v6/bars.inc`, 2025-04-01 00:00 .. 2025-04-17 00:00
UTC), with TradingView's 0.0001 lot as the `qty_step`, and requires every trade
the tape closes inside the bars -- entry and exit time, side, price in ticks of
0.01, quantity in lots -- to be the engine's.
