# Where W10's and W5B's margin rules meet at one open (lane INT29)

INT29 puts lane W10-DIAG-UNKNOWN onto main, which already carries lane
W5B-ENG-MARGIN-RESIDUAL. Their fill-time margin rules meet in
`PineExecutionAdapter::validate_precommit`:

| rule | lane | what TradingView does |
|---|---|---|
| SB | W5B | a flat bar's same-side MARKET entries each fill on their own cost; the book is called once they have |
| PA | W5B | an add is judged on its combined margin, with no lot of slack |
| COQ | W10 | a MARKET entry filled at the open after its bar is judged against the position held when it was placed, not a sibling that filled at that open a moment earlier |
| SAMEOPEN-REV | W10 | a default-size MARKET entry that reverses a position its own bar's entry opened at that open must fit with that position still margined |

`int29-c1-open-pairs` is one `lab tv --no-note` export of a synthetic probe
written for INT29 (channel `ws-report-v1`, `rangeProof` covered), byte for
byte: `strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. It runs on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with
v6's defaults (100000, margins of 100), a default size of 30 % of equity and
`pyramiding = 4`. A cell every 2 hours from 2025-04-02 00:00 UTC, 120 cells,
five kinds in turn, each flattened by a `close_all` placed 45 minutes in:

| kind | orders | TradingView, all 24 cells of the kind |
|---|---|---|
| A (SB) | from flat, two longs of 60 % on one bar | both fill at the next open; the call there takes the first whole and the second in part |
| B (COQ) | a seed long of 30 %, then two adds of 40 % on one bar | both adds fill at one open; the call takes the seed whole and the first add in part |
| C (SB + SAMEOPEN-REV) | from flat, a long of 80 %, a default short and a long of 30 % on one bar | both longs fill at one open; the short is refused; the call takes part of the first long |
| D (COQ + PA) | a seed long of 50 %, then an add of 30 % and a default add on one bar | both adds fill at one open; the call takes part of the seed |
| E (PA pair) | a seed long of 50 %, then two default adds on one bar | both adds fill at one open; the call takes part of the seed |

Cells D and E are the composition neither lane's tapes held: an add sized by
the default percentage goes through validate_precommit's frozen-sizing arm
(PA's), not the explicit arm COQ was written in. TradingView judges it as COQ
judges an explicit one, on the position held at its placement and itself,
with no lot of slack; the frozen arm now does so (INT29's composition
commit). Before it, the integrated tree refused the second add of every D and
E cell: `test_int29_rule_compositions` failed (2432 passed, 1 failed; the
engine booked 336 of the tape's 432 trades, first difference in cell D).

In the lanes' own tapes the rules already act together on this tree (an
instrumented copy of the adapter, scratch only): `w10-coq-flat-pair`
(tests/fixtures/coqueued_open_margin) runs SB three times (cells A, B, C) and
COQ once (cell D), and `w10-sameopen-shapes-base`
(tests/fixtures/same_open_reversal) runs SAMEOPEN-REV 33 times and SB 4
times; both replays pass.

| tape | tv_trades.csv sha256 | strategy.pine sha256 |
|---|---|---|
| `int29-c1-open-pairs` | `08d7b77d89d5c1985b5f3d6cf34e7ac0ed28b394fa7944cce8dc75ce5f2d768d` | `67ce3bd1cc36619796726317bd011b84f3d5a078acfd66922ecf356eb1501d89` |

`tests/test_int29_rule_compositions.cpp` replays the tape through the Pine
adapter under the configuration the generated constructor declares, trading
from the bar that places the first cell, with TradingView's 0.0001 lot as the
`qty_step`, and requires each trade the tape closes inside the replayed bars
(entry and exit time, side, price in ticks of 0.01, quantity in lots of
0.0001) to be the engine's. It also reads each kind's shape off TradingView's
own rows.

`bars.inc` holds the corpus 15m chart feed rows 2025-04-01 00:00 ..
2025-04-12 12:00 UTC (sha256
`27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`, derived
from the corpus 1m feed
`db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`).
