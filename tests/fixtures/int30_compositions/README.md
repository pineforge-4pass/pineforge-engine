# Where TAIL-B's calls for a bar's adds meet TAIL-H's rule PK (INT30)

INT30 puts lane TAIL-B and lane TAIL-H onto main, which already carries lane
W13-ENG-MARGIN-OPP. Their rules meet in `PineExecutionAdapter::on_applied`:

| rule | lane | what TradingView does |
|---|---|---|
| AS | TAIL-B | each MARKET add a bar places on a held long is judged against the position held when it was placed plus its own units |
| AC | TAIL-B | the grown book is margin-called once, after the last add: at that fill for a market add, at the next open, sized at the close, under `process_orders_on_close` |
| LF | TAIL-B | after a call that closes several lots of a long, the book is checked again after each lot, and the last check still short is called again |
| PK | TAIL-H | once a margin call has shrunk the position, the exits hold the position that remains in their queue order again: a 50 % exit keeps the share it reserved, and a stop exit created behind it gets what that leaves |

Every call AC or LF takes applies through `on_applied` as a Margin-family fill,
and PK re-reserves the exits of the entry it shrank there, before LF sizes its
follow-up.

Each tape is one `lab tv --no-note` export of a synthetic probe written for
INT30 (channel `ws-report-v1`, `rangeProof` covered), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. Both run on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with
10000 of capital, `pyramiding = 200` and margins of 100. A cell every 6 hours
from 2025-04-02 00:00 UTC, forty cells: on a cell's first bar, flat,
E = initial capital + net profit rounded to 100; n = 1, 3 or 10 lots, one per
bar, the n - 1 older seeds sharing 0.3 E and the newest, `L`, 0.6 E (0.9 E
alone); on the next bar `L`'s exits TP (50 %, limit 0.6 % above the close)
and SL (stop 0.6 % below), TP first or SL first by cell; two bars later three
adds of 0.07 E on one bar; a `close_all` on the cell's 23rd bar.

| tape | strategy() | TradingView |
|---|---|---|
| `int30-c4-grid-pk-mkt` | market, commission 0.1 %, slippage 2 | 351 trades; the adds fill in all 40 cells; 23 cells are called (111 call rows, 16 calls close two lots or more, no follow-up call); after the call `L` exits by SL in 13 cells and by TP in 2 |
| `int30-c4-grid-pk-pooc` | `process_orders_on_close`, commission 0.06 %, slippage 3 | the same counts, at the closes and the next opens |

`tests/test_int30_rule_compositions.cpp` replays both through the Pine adapter
over the 15m bars of `tests/fixtures/margin_v6/bars.inc` and requires every
trade the tape closes inside those bars to be the engine's. The
`process_orders_on_close` tape matches trade for trade. The market tape
matches but for one recorded
call, which is AC's sizing price and not the composition: at margins of 100
a book's shortfall is its cost plus its fees less the equity, whatever the
mark; TradingView calls four times its lot floor over the print the adds'
fill slipped from (or over the call's own price, a tick under it), and AC
divides by the slipped fill. In cell 37 (2025-04-11 07:00 UTC) the two
quotients straddle a lot: TradingView calls 2.1404 of `L`, the engine
2.1400, and SL, which holds what the call leaves, exits 2.3111 on the tape
and 2.3115 in the engine. The same boundary appeared in 2 of the 23 called
market cells of the unrounded probe (E not rounded) and never under
`process_orders_on_close`. PK's split is TradingView's after every call.

The test fails both tapes from their first trade on every tree that lacks one
side: engine main 7239ab37 and lane TAIL-H's final tree 647c9db7 (no AS, AC or
LF: 352 trades against the market tape's 351, 283 against the other's 351),
and lane TAIL-B's final tree f95385fb (no PK: 358 and 359 against 351).

The probe keeps its exits on one lot. An earlier probe seeded `L` itself in
several lots; TradingView then books the 50 % exit and the stop per lot of
`L` (each lot's share floored on its own, and one closed row per lot and
exit order), where the engine books one exit per entry. That difference
predates INT30: in a cell without a call, main and the integrated tree book
identical trades there.

| tape | trades | tv_trades.csv sha256 | strategy.pine sha256 (12) |
|---|---:|---|---|
| `int30-c4-grid-pk-mkt` | 351 | `d2d7b731f1a02b4df3500101a0cee37bbf812f52b1105a90ca2f7f7d20ed7b15` | `0355d9d94065` |
| `int30-c4-grid-pk-pooc` | 351 | `0c9a27cb1debe2a7cd35355762c5a2809ccceba86f23678a4f70b83c7b8fe30b` | `375a109315b8` |
