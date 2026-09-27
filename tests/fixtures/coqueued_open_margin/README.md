# MARKET entries filled at one open are each judged alone (lane W10-DIAG-UNKNOWN, rule COQ)

TradingView judges a MARKET entry it fills at a bar's open against the
position held when the entry was placed. A sibling entry that filled at the
same open a moment earlier does not count against it: two entries placed on
one bar that each fit the equity alone both fill, even when together they do
not, and the excess margin is then called at the fill, first in, first out.
Pyramiding still caps the entries: an entry the open trades already hold at the
cap is refused (lane H-MEASURE's first-eligible-point count).

The population probe that showed it is `pf-probe-thula-live-explicit-add-free-funds`
(six lanes, weak on all of them): its co-queued cells place two 55% entries
from flat. The adapter's fill-time admission counted the first entry's lot as
held when it judged the second, and refused it; TradingView fills both and
calls 0.48432 of the first on BINANCE:BTCUSDT at the fill price.

Each directory is one `lab tv --no-note` export of a synthetic probe written
for this lane (channel `ws-report-v1`, `rangeProof` covered), byte for byte:
`strategy.pine`, `tv_trades.csv` (times at UTC+8), `metrics.json`,
`meta.json`. Both run on BINANCE:ETHUSDT.P 15, 2025-04-01 .. 2025-05-01, with
fixed `timestamp("UTC", ...)` cells on 2025-04-08 and 2025-04-09 and a
`strategy.cancel_all()` plus `strategy.close_all()` cleanup an hour after each
cell, `pyramiding = 4`, margins of 100 and v6's 100000 capital.

| tape | `strategy()` declares | tv_trades.csv sha256 |
|---|---|---|
| `w10-coq-flat-pair` | nothing (v6: percent_of_equity, 100); every entry names its qty | `300f534b2792b963c5223641d9d78bb9547f82f039ddece360f6f937605b7117` |
| `w10-coq-flat-pair-fixed` | `default_qty_type = strategy.fixed, default_qty_value = 34`; the A cell's two entries omit qty | `300f534b2792b963c5223641d9d78bb9547f82f039ddece360f6f937605b7117` |

The two tapes are one file: a default fixed size is admitted like a named one.

| cell | orders | TradingView |
|---|---|---|
| A (04-08 00:00) | from flat, two longs of 34 (together over the equity) | both fill at 1559.12; 15.4448 of the first is margin-called at the fill |
| B (04-08 04:00) | from flat, two shorts of 33 | both fill at 1588.37; 13.5616 of the first is called at the fill |
| C (04-08 08:00) | from flat, two longs of 30 (together inside the equity) | both fill whole, no call (control) |
| D (04-09 00:00, 00:15) | a seed long of 20, then two adds of 28 on one bar (each fits alone) | both adds fill at 1473.01; the call takes the seed whole and 12.4296 of the first add |
| E (04-09 04:00, 04:15) | a seed long of 20, then one add of 56 (over the equity) | the add is refused (control) |

`tests/test_coqueued_open_margin_tapes.cpp` replays both tapes through the Pine
adapter under the configuration the generated constructor declares, trading
from the bar that places cell A (2025-04-08 00:00 UTC) as `run_strategy.py`
does for a corpus tape, with TradingView's 0.0001 lot as the `qty_step`, and
requires each trade the tape closes inside the replayed bars (entry and exit
time, side, price in ticks of 0.01, quantity in lots of 0.0001) to be the
engine's. It also reads the rule off TradingView's own rows. On the base
8633d944 it fails both replays (155 passed, 2 failed): the adapter books one
entry of each over-equity pair and one add of cell D.

`bars.inc` holds the corpus 15m chart feed rows 2025-04-07 00:00 .. 2025-04-09
12:00 UTC (sha256 `27b62431096edf1bfba71b2409f8dc183f69213dec2834560b3241750f8e7026`,
derived from the corpus 1m feed `db8c1332da093008cfbd063e05db0b33fe8f7fd35d78cf058a366519eb9f6cc5`).
